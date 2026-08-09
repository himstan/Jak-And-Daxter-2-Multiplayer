#include "game/multiplayer/multiplayer_security.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <sodium.h>
#include <string_view>

#include "game/multiplayer/multiplayer_version.h"
#include "game/multiplayer/multiplayer_preferences.h"
#include "game/multiplayer/multiplayer_wire_codec.h"

namespace {
constexpr std::array<uint8_t, 4> kMagic = {'O', 'G', 'M', '1'};
constexpr std::string_view kRoomCodeAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
constexpr uint8_t kServerHello = 1;
constexpr uint8_t kClientProof = 2;
constexpr uint8_t kServerProof = 3;
constexpr uint8_t kVersionMismatch = 4;
constexpr uint8_t kEncryptedGameplay = 5;
constexpr size_t kBaseHeaderSize = 6;
constexpr size_t kEncryptedHeaderSize = 34;
constexpr size_t kAeadTagSize = crypto_aead_xchacha20poly1305_ietf_ABYTES;

void write_base_header(uint8_t* output, uint8_t kind) {
  memcpy(output, kMagic.data(), kMagic.size());
  output[4] = kind;
  output[5] = 0;
}

bool valid_base_header(const uint8_t* data, size_t size, uint8_t kind) {
  return data && size >= kBaseHeaderSize &&
         sodium_memcmp(data, kMagic.data(), kMagic.size()) == 0 &&
         data[4] == kind;
}

bool append_version(MultiplayerDatagram& datagram, size_t& offset, const std::string& version) {
  if (!mp_valid_compatibility_identity(version) || version.size() > UINT8_MAX ||
      offset > datagram.bytes.size() - 1 - version.size()) {
    return false;
  }
  datagram.bytes[offset++] = static_cast<uint8_t>(version.size());
  memcpy(datagram.bytes.data() + offset, version.data(), version.size());
  offset += version.size();
  return true;
}

bool read_version(const uint8_t* data, size_t size, size_t& offset, std::string& version) {
  if (!data || offset >= size) {
    return false;
  }
  const size_t length = data[offset++];
  if (length == 0 || length > kMultiplayerVersionMaxLength || length > size - offset) {
    return false;
  }
  version.assign(reinterpret_cast<const char*>(data + offset), length);
  offset += length;
  if (!mp_valid_compatibility_identity(version)) {
    version.clear();
    return false;
  }
  return true;
}

void hash_version(crypto_generichash_state& hash, const std::string& version) {
  const uint8_t length = static_cast<uint8_t>(version.size());
  crypto_generichash_update(&hash, &length, sizeof(length));
  crypto_generichash_update(&hash, reinterpret_cast<const uint8_t*>(version.data()),
                            version.size());
}

bool valid_ipv4_address(std::string_view address) {
  size_t start = 0;
  for (int component = 0; component < 4; ++component) {
    const size_t end = component == 3 ? address.size() : address.find('.', start);
    if (end == std::string_view::npos || end == start || end - start > 3) {
      return false;
    }
    uint32_t value = 0;
    const auto result = std::from_chars(address.data() + start, address.data() + end, value);
    if (result.ec != std::errc() || result.ptr != address.data() + end || value > 255) {
      return false;
    }
    start = end + 1;
  }
  return start == address.size() + 1;
}

void shift_replay_window(std::array<uint64_t, 4>& window, uint64_t distance) {
  if (distance >= 256) {
    window.fill(0);
    return;
  }
  const size_t word_shift = static_cast<size_t>(distance / 64);
  const size_t bit_shift = static_cast<size_t>(distance % 64);
  std::array<uint64_t, 4> shifted = {};
  for (size_t destination = 4; destination-- > 0;) {
    if (destination < word_shift) {
      continue;
    }
    const size_t source = destination - word_shift;
    shifted[destination] |= window[source] << bit_shift;
    if (bit_shift != 0 && source > 0) {
      shifted[destination] |= window[source - 1] >> (64 - bit_shift);
    }
  }
  window = shifted;
}
}  // namespace

MultiplayerSecurity::~MultiplayerSecurity() {
  clear_secrets();
}

bool MultiplayerSecurity::initialize_sodium() {
  static const bool initialized = sodium_init() >= 0;
  return initialized;
}

bool MultiplayerSecurity::start_host(uint16_t port, const std::string& room_code) {
  reset();
  if (!initialize_sodium() || port == 0) {
    return false;
  }
  m_host = true;
  m_port = port;
  randombytes_buf(m_session_id.data(), m_session_id.size());
  randombytes_buf(m_server_nonce.data(), m_server_nonce.size());
  randombytes_buf(m_credential_salt.data(), m_credential_salt.size());

  std::string selected_room_code;
  if (!room_code.empty()) {
    if (!mp_normalize_room_code(room_code, selected_room_code, false)) {
      reset();
      return false;
    }
  } else {
    selected_room_code.resize(kMultiplayerRoomCodeLength);
    for (char& character : selected_room_code) {
      character = kRoomCodeAlphabet[randombytes_uniform(
          static_cast<uint32_t>(kRoomCodeAlphabet.size()))];
    }
  }

  if (!derive_room_code_key(selected_room_code)) {
    mp_secure_clear_string(selected_room_code);
    reset();
    return false;
  }
  m_room_code = std::move(selected_room_code);
  return true;
}

bool MultiplayerSecurity::derive_room_code_key(std::string_view room_code) {
  return crypto_pwhash(m_credential_key.data(), m_credential_key.size(), room_code.data(),
                       room_code.size(),
                       m_credential_salt.data(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                       crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) == 0;
}

bool MultiplayerSecurity::start_client(const std::string& invite,
                                       std::string& host,
                                       uint16_t& port) {
  reset();
  if (!initialize_sodium() || !parse_invite(invite, host, port)) {
    reset();
    host.clear();
    port = 0;
    return false;
  }
  m_host = false;
  m_port = port;
  return true;
}

bool MultiplayerSecurity::set_local_version(const std::string& version) {
  if (!mp_valid_compatibility_identity(version)) {
    return false;
  }
  m_local_version = version;
  return true;
}

bool MultiplayerSecurity::rotate_host_peer_session() {
  if (!m_host || m_room_code.empty() || !mp_valid_compatibility_identity(m_local_version) ||
      !initialize_sodium()) {
    return false;
  }

  clear_peer_secrets();
  m_authenticated = false;
  m_send_counter = 0;
  m_highest_received_counter = 0;
  m_replay_window.fill(0);
  m_client_nonce.fill(0);
  mp_secure_clear_string(m_remote_version);
  randombytes_buf(m_session_id.data(), m_session_id.size());
  randombytes_buf(m_server_nonce.data(), m_server_nonce.size());
  return true;
}

void MultiplayerSecurity::reset() {
  clear_secrets();
  m_host = false;
  m_authenticated = false;
  m_port = 0;
  m_send_counter = 0;
  m_highest_received_counter = 0;
  m_replay_window.fill(0);
  m_session_id.fill(0);
  m_server_nonce.fill(0);
  m_client_nonce.fill(0);
  m_room_code.clear();
  m_credential_salt.fill(0);
  m_local_version.clear();
  mp_secure_clear_string(m_remote_version);
}

bool MultiplayerSecurity::authenticated() const {
  return m_authenticated;
}

const std::string& MultiplayerSecurity::room_code() const {
  return m_room_code;
}

const std::string& MultiplayerSecurity::remote_version() const {
  return m_remote_version;
}

std::string MultiplayerSecurity::invite_for_address(const std::string& address) const {
  if (address.empty() || m_room_code.empty() || m_port == 0) {
    return {};
  }
  return "jad2mp://" + address + ":" + std::to_string(m_port) + "/" + m_room_code;
}

bool MultiplayerSecurity::parse_invite(const std::string& invite,
                                       std::string& host,
                                       uint16_t& port) {
  constexpr std::string_view prefix = "jad2mp://";
  constexpr size_t kMaximumInviteLength = 37;
  if (!invite.starts_with(prefix) || invite.size() > kMaximumInviteLength) {
    return false;
  }
  const size_t room_code_separator = invite.find('/', prefix.size());
  const size_t port_separator = invite.rfind(':', room_code_separator);
  if (room_code_separator == std::string::npos || port_separator == std::string::npos ||
      port_separator == prefix.size() ||
      invite.find('/', room_code_separator + 1) != std::string::npos) {
    return false;
  }

  host = invite.substr(prefix.size(), port_separator - prefix.size());
  const std::string_view port_text(invite.data() + port_separator + 1,
                                   room_code_separator - port_separator - 1);
  uint32_t parsed_port = 0;
  const auto port_result =
      std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
  if (!valid_ipv4_address(host) || port_result.ec != std::errc() ||
      port_result.ptr != port_text.data() + port_text.size() || parsed_port == 0 ||
      parsed_port > (std::numeric_limits<uint16_t>::max)()) {
    return false;
  }

  const std::string_view room_code_text(invite.data() + room_code_separator + 1,
                                        invite.size() - room_code_separator - 1);
  std::string normalized;
  if (!mp_normalize_room_code(room_code_text, normalized, false)) {
    return false;
  }
  m_room_code = std::move(normalized);
  port = static_cast<uint16_t>(parsed_port);
  return true;
}

bool MultiplayerSecurity::make_server_hello(MultiplayerDatagram& output) const {
  if (!m_host || m_room_code.empty() || !mp_valid_compatibility_identity(m_local_version)) {
    return false;
  }
  size_t offset = kBaseHeaderSize;
  write_base_header(output.bytes.data(), kServerHello);
  memcpy(output.bytes.data() + offset, m_credential_salt.data(), m_credential_salt.size());
  offset += m_credential_salt.size();
  memcpy(output.bytes.data() + offset, m_session_id.data(), m_session_id.size());
  offset += m_session_id.size();
  memcpy(output.bytes.data() + offset, m_server_nonce.data(), m_server_nonce.size());
  offset += m_server_nonce.size();
  if (!append_version(output, offset, m_local_version)) {
    return false;
  }
  output.size = offset;
  return true;
}

void MultiplayerSecurity::make_proof(const char* label,
                                     std::array<uint8_t, kKeySize>& proof) const {
  crypto_generichash_state hash = {};
  crypto_generichash_init(&hash, m_credential_key.data(), m_credential_key.size(), proof.size());
  crypto_generichash_update(&hash, reinterpret_cast<const uint8_t*>(label), strlen(label));
  crypto_generichash_update(&hash, m_session_id.data(), m_session_id.size());
  crypto_generichash_update(&hash, m_server_nonce.data(), m_server_nonce.size());
  crypto_generichash_update(&hash, m_client_nonce.data(), m_client_nonce.size());
  const std::string& host_version = m_host ? m_local_version : m_remote_version;
  const std::string& client_version = m_host ? m_remote_version : m_local_version;
  hash_version(hash, host_version);
  hash_version(hash, client_version);
  crypto_generichash_final(&hash, proof.data(), proof.size());
}

void MultiplayerSecurity::derive_session_keys(int session_role) {
  std::array<uint8_t, kKeySize> host_key = {};
  std::array<uint8_t, kKeySize> client_key = {};
  std::array<uint8_t, kKeySize> host_nonce_material = {};
  std::array<uint8_t, kKeySize> client_nonce_material = {};
  make_proof("host-key", host_key);
  make_proof("client-key", client_key);
  make_proof("host-nonce", host_nonce_material);
  make_proof("client-nonce", client_nonce_material);
  const bool local_is_host = session_role == 0;
  m_send_key = local_is_host ? host_key : client_key;
  m_receive_key = local_is_host ? client_key : host_key;
  const auto& send_nonce = local_is_host ? host_nonce_material : client_nonce_material;
  const auto& receive_nonce = local_is_host ? client_nonce_material : host_nonce_material;
  std::copy_n(send_nonce.begin(), m_send_nonce_prefix.size(), m_send_nonce_prefix.begin());
  std::copy_n(receive_nonce.begin(), m_receive_nonce_prefix.size(), m_receive_nonce_prefix.begin());
  sodium_memzero(host_key.data(), host_key.size());
  sodium_memzero(client_key.data(), client_key.size());
  sodium_memzero(host_nonce_material.data(), host_nonce_material.size());
  sodium_memzero(client_nonce_material.data(), client_nonce_material.size());
}

SecurityReceiveResult MultiplayerSecurity::receive(int session_role,
                                                   const uint8_t* data,
                                                   size_t size) {
  SecurityReceiveResult result;
  if (!data || size < kBaseHeaderSize) {
    return result;
  }

  if (!m_authenticated && session_role == 1 && valid_base_header(data, size, kServerHello)) {
    size_t offset = kBaseHeaderSize;
    if (size < offset + m_credential_salt.size() + m_session_id.size() +
                   m_server_nonce.size() + 1 ||
        !mp_valid_compatibility_identity(m_local_version)) {
      return result;
    }
    memcpy(m_credential_salt.data(), data + offset, m_credential_salt.size());
    offset += m_credential_salt.size();
    if (!derive_room_code_key(m_room_code)) {
      return result;
    }
    memcpy(m_session_id.data(), data + offset, m_session_id.size());
    offset += m_session_id.size();
    memcpy(m_server_nonce.data(), data + offset, m_server_nonce.size());
    offset += m_server_nonce.size();
    if (!read_version(data, size, offset, m_remote_version) || offset != size) {
      return result;
    }

    randombytes_buf(m_client_nonce.data(), m_client_nonce.size());
    std::array<uint8_t, kKeySize> proof = {};
    make_proof("client-proof", proof);
    write_base_header(result.response.bytes.data(), kClientProof);
    offset = kBaseHeaderSize;
    memcpy(result.response.bytes.data() + offset, m_session_id.data(), m_session_id.size());
    offset += m_session_id.size();
    memcpy(result.response.bytes.data() + offset, m_client_nonce.data(), m_client_nonce.size());
    offset += m_client_nonce.size();
    if (!append_version(result.response, offset, m_local_version) ||
        offset > result.response.bytes.size() - proof.size()) {
      sodium_memzero(proof.data(), proof.size());
      return result;
    }
    memcpy(result.response.bytes.data() + offset, proof.data(), proof.size());
    offset += proof.size();
    sodium_memzero(proof.data(), proof.size());
    result.response.size = offset;
    result.kind = SecurityReceiveKind::HANDSHAKE;
    return result;
  }

  if (!m_authenticated && session_role == 0 && valid_base_header(data, size, kClientProof)) {
    size_t offset = kBaseHeaderSize;
    if (size < offset + m_session_id.size() + m_client_nonce.size() + 1 + kKeySize ||
        sodium_memcmp(data + offset, m_session_id.data(), m_session_id.size()) != 0) {
      return result;
    }
    offset += m_session_id.size();
    memcpy(m_client_nonce.data(), data + offset, m_client_nonce.size());
    offset += m_client_nonce.size();
    if (!read_version(data, size, offset, m_remote_version) || size - offset != kKeySize) {
      return result;
    }

    std::array<uint8_t, kKeySize> expected = {};
    make_proof("client-proof", expected);
    if (sodium_memcmp(data + offset, expected.data(), expected.size()) != 0) {
      sodium_memzero(expected.data(), expected.size());
      return result;
    }
    sodium_memzero(expected.data(), expected.size());

    const bool versions_match = m_local_version == m_remote_version;
    const char* proof_label = versions_match ? "server-proof" : "version-mismatch";
    const uint8_t response_kind = versions_match ? kServerProof : kVersionMismatch;
    std::array<uint8_t, kKeySize> server_proof = {};
    make_proof(proof_label, server_proof);
    write_base_header(result.response.bytes.data(), response_kind);
    offset = kBaseHeaderSize;
    memcpy(result.response.bytes.data() + offset, m_session_id.data(), m_session_id.size());
    offset += m_session_id.size();
    if (!append_version(result.response, offset, m_local_version) ||
        offset > result.response.bytes.size() - server_proof.size()) {
      sodium_memzero(server_proof.data(), server_proof.size());
      return result;
    }
    memcpy(result.response.bytes.data() + offset, server_proof.data(), server_proof.size());
    offset += server_proof.size();
    sodium_memzero(server_proof.data(), server_proof.size());
    result.response.size = offset;

    if (!versions_match) {
      result.kind = SecurityReceiveKind::VERSION_MISMATCH;
      return result;
    }
    derive_session_keys(session_role);
    m_authenticated = true;
    result.kind = SecurityReceiveKind::HANDSHAKE;
    return result;
  }

  if (!m_authenticated && session_role == 1 &&
      (valid_base_header(data, size, kServerProof) ||
       valid_base_header(data, size, kVersionMismatch))) {
    const bool mismatch = data[4] == kVersionMismatch;
    size_t offset = kBaseHeaderSize;
    if (size < offset + m_session_id.size() + 1 + kKeySize ||
        sodium_memcmp(data + offset, m_session_id.data(), m_session_id.size()) != 0) {
      return result;
    }
    offset += m_session_id.size();
    std::string supplied_host_version;
    if (!read_version(data, size, offset, supplied_host_version) ||
        supplied_host_version != m_remote_version || size - offset != kKeySize) {
      return result;
    }
    std::array<uint8_t, kKeySize> expected = {};
    make_proof(mismatch ? "version-mismatch" : "server-proof", expected);
    if (sodium_memcmp(data + offset, expected.data(), expected.size()) != 0) {
      sodium_memzero(expected.data(), expected.size());
      return result;
    }
    sodium_memzero(expected.data(), expected.size());
    if (mismatch) {
      result.kind = SecurityReceiveKind::VERSION_MISMATCH;
      return result;
    }
    if (m_local_version != m_remote_version) {
      return result;
    }
    derive_session_keys(session_role);
    m_authenticated = true;
    result.kind = SecurityReceiveKind::HANDSHAKE;
    return result;
  }

  if (!m_authenticated || !valid_base_header(data, size, kEncryptedGameplay) ||
      size < kEncryptedHeaderSize + kAeadTagSize ||
      sodium_memcmp(data + 10, m_session_id.data(), m_session_id.size()) != 0) {
    return result;
  }
  const uint8_t remote_direction = session_role == 0 ? 1 : 0;
  const uint16_t plaintext_size = multiplayer::wire::load_u16_le(data + 8);
  const uint64_t counter = multiplayer::wire::load_u64_le(data + 26);
  if (data[6] != remote_direction || data[7] >= static_cast<uint8_t>(PacketType::COUNT) ||
      plaintext_size > result.plaintext.bytes.size() ||
      size != kEncryptedHeaderSize + plaintext_size + kAeadTagSize) {
    return result;
  }

  std::array<uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce = {};
  memcpy(nonce.data(), m_receive_nonce_prefix.data(), m_receive_nonce_prefix.size());
  multiplayer::wire::store_u64_le(nonce.data() + m_receive_nonce_prefix.size(), counter);
  unsigned long long decrypted_size = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(
          result.plaintext.bytes.data(), &decrypted_size, nullptr, data + kEncryptedHeaderSize,
          size - kEncryptedHeaderSize, data, kEncryptedHeaderSize, nonce.data(),
          m_receive_key.data()) != 0 ||
      decrypted_size != plaintext_size || !accept_counter(counter)) {
    result.plaintext.size = 0;
    return result;
  }
  if (decrypted_size < sizeof(PacketHeader) || result.plaintext.bytes[0] != data[7]) {
    result.plaintext.size = 0;
    return result;
  }
  result.plaintext.size = static_cast<size_t>(decrypted_size);
  result.kind = SecurityReceiveKind::GAMEPLAY;
  return result;
}

bool MultiplayerSecurity::seal(int session_role,
                               PacketType packet_type,
                               const void* plaintext,
                               size_t plaintext_size,
                               MultiplayerDatagram& output) {
  if (!m_authenticated || !plaintext || plaintext_size == 0 || packet_type >= PacketType::COUNT ||
      plaintext_size > kMultiplayerMaxDatagramSize - kEncryptedHeaderSize - kAeadTagSize ||
      m_send_counter == (std::numeric_limits<uint64_t>::max)()) {
    return false;
  }

  const uint64_t counter = ++m_send_counter;
  write_base_header(output.bytes.data(), kEncryptedGameplay);
  output.bytes[6] = static_cast<uint8_t>(session_role);
  output.bytes[7] = static_cast<uint8_t>(packet_type);
  multiplayer::wire::store_u16_le(output.bytes.data() + 8, static_cast<uint16_t>(plaintext_size));
  memcpy(output.bytes.data() + 10, m_session_id.data(), m_session_id.size());
  multiplayer::wire::store_u64_le(output.bytes.data() + 26, counter);

  std::array<uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce = {};
  memcpy(nonce.data(), m_send_nonce_prefix.data(), m_send_nonce_prefix.size());
  multiplayer::wire::store_u64_le(nonce.data() + m_send_nonce_prefix.size(), counter);
  unsigned long long encrypted_size = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          output.bytes.data() + kEncryptedHeaderSize, &encrypted_size,
          static_cast<const uint8_t*>(plaintext), plaintext_size, output.bytes.data(),
          kEncryptedHeaderSize, nullptr, nonce.data(), m_send_key.data()) != 0) {
    return false;
  }
  output.size = kEncryptedHeaderSize + static_cast<size_t>(encrypted_size);
  return true;
}

bool MultiplayerSecurity::accept_counter(uint64_t counter) {
  if (counter == 0) {
    return false;
  }
  if (counter > m_highest_received_counter) {
    const uint64_t distance = counter - m_highest_received_counter;
    shift_replay_window(m_replay_window, distance);
    m_highest_received_counter = counter;
    m_replay_window[0] |= 1;
    return true;
  }
  const uint64_t age = m_highest_received_counter - counter;
  if (age >= 256) {
    return false;
  }
  const size_t word = static_cast<size_t>(age / 64);
  const uint64_t bit = uint64_t{1} << (age % 64);
  if ((m_replay_window[word] & bit) != 0) {
    return false;
  }
  m_replay_window[word] |= bit;
  return true;
}

void MultiplayerSecurity::clear_peer_secrets() {
  sodium_memzero(m_send_key.data(), m_send_key.size());
  sodium_memzero(m_receive_key.data(), m_receive_key.size());
  sodium_memzero(m_send_nonce_prefix.data(), m_send_nonce_prefix.size());
  sodium_memzero(m_receive_nonce_prefix.data(), m_receive_nonce_prefix.size());
}

void MultiplayerSecurity::clear_secrets() {
  clear_peer_secrets();
  sodium_memzero(m_credential_key.data(), m_credential_key.size());
  sodium_memzero(m_credential_salt.data(), m_credential_salt.size());
  if (!m_room_code.empty()) {
    sodium_memzero(m_room_code.data(), m_room_code.size());
  }
}

void mp_secure_clear_string(std::string& value) {
  if (!value.empty()) {
    sodium_memzero(value.data(), value.size());
  }
  value.clear();
}
