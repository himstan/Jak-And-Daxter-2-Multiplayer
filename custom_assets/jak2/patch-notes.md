# 0.3.3
-# Released 2026 November 19
## Added
- **Patch Notes tab**: This very menu you're reading

# 0.3.2
-# Released 2026 November 18
## Added
- **Chat typing indicator**: Whenever a player is typing you'll see a ... above their head
## Changed
- **Onin Game**: Lowered the penalty for missing symbols to 10
## Fixed
- **Remote scene starts**: Some scenes left you in a black screen if it was a remote player triggering it
- **Floating Daxter jetboards in scenes**: In some cutscene you could see floating jetboards that were left there from Daxter players
- **Whack minigame**: Previously the challenge didn't complete for the Host if another player won
- **Daxter getting soft locked during the last boss fight**: The game pushed Daxters into some weird soft-locked Dark Jak mode
- **Ambushing or Hiding enemies were invisible for clients**: During certain battles some enemies stayed invisible for clients
- **Chat line wrapping**: Long chat messages get properly wrapped now
- **Mammoth stuck in waiting**: Whenever a mammoth got passed between two players it got stuck in waiting
- **Game Crashes**: Fixed a bunch of crashes
  - **Underport Sig**: If the lobby had a Daxter the Host crashed after the ending cutscene
  - **Poles**: Could crash you if it had a player on it while your client deloaded the pole
  - **And more..**

# 0.3.1
-# Released 2026 November 15
## Added
- **Vehicle honk**: You can honk the vehicle you're driving by pressing circle
- **Side Missions/Burning Bush**: All challenges have received patches so they all should behave in a multiplayer focused way **Only the Host can trigger these. If you're a client and are far away from the challenge then you will be teleported.**
  - **Orb Search**: Any player can reach the orb and it will be counted
  - **Ring race**: These will only fail if **all** players fail the challenge, if at least one player finishes it the rewards are given and the challenge is completed
  - **Collection**: These generic collection mission will all work, you can split the work between you, the more players the lobby has the easier these become, they do not scale difficulty with player count
  - **Stadium Races**: Stadium races can be started by the Host and it will be activated for all players
## Fixed
- **In-Game Menu Ring**: The menu stays attached to your camera even if you're moving
- **Gun Buoy/Water Robot Guard**: These now only shoot the player that has triggered them
- **Daxter Crash**: A Daxter player no longer crashes when getting hit while swimming
- **Daxter entering vehicles from Jetboard**: Previously it dropped the indax mode from the player, so it looked as if he was sitting on top of an invisible Jak
- **Mountain Transport Platform**: Now it will wait for all players not in the forest and alive before departing
- **Pumping Station Turbine Sync**: The turbines now calculate their rotation using the shared clock
- **Protect site in Dead Town**: The last two grenadiers now just get woken from the get-go, previously they were listening for an event to come out from their dormant state which was missed sometimes
- **Mission Fail Retry Secrets**: Now if there's a Mission Failed retry the clients don't get their visual secrets reset

# 0.3.0
-# Released 2026 November 14
## Added
- **Act 2 and 3**: The whole game should be playable now (with a bunch of bugs I'm sure)
- **Hero Mode**: If the Host has a session in hero mode, the clients will be told about it now.
- **Extended Chat History**: Chat entries stay longer and now the history can be looked back at with **PageUp**/**PageDown**
## Changed
- **Debug Permissions**: Clients will only be able to open their Debug Menu if the Host is in debug mode, and can only use cheats if the Host has given permission (Multiplayer -> Debug Permissions)
- **Mountain Collection Notifications**: Since during this mission you can split up on the three paths, now players will receive a notifications if one of the three items have been collected by someone
- **Hellcat health**: Player driven hellcats now have double their default health, so they shouldn't get blown up by one kamikaze guard bike
## Fixed
- **Stuck on city screen when quitting from a session**: Quit Game should always bring you back to the title menu now
- **Warp Gates**: Other players coming in and out of warp gates should not look so janky anymore

# Update 0.2.1
-# Released 2026 October 25
## Fixed
- **Late joiners during races**: Now if someone joins a game mid race the race wont be interrupted, they'll just spectate the game until the race ends
- **Oracle and Dark Jak fixes**: If someone else turns into Dark Jak they wont steal your precious dark eco, also the Oracle can't hold you hostage as Dark Jak anymore
- **Crash fixes**: The rampant crash that happened with more than 4-5 players is fixed, and Daxter can't attempt to wall-hide anymore which caused some crashes for observers too

# Update 0.2.0
-# Released 2026 October 24
## Added
- **Steam Integration (opt-in)**: Boots the game as Spacewar, please only enable this if that doesn't bother you. Allows public lobbies, host without port forwarding (since it uses the steam servers)
- **In-game Text Chat**: You can chat by pressing **T**
- **First start tooltips**: When entering the game for the very first time you'll see some MP specific tutorial popups (only once)
- **Guard vehicles**: Previously guard vehicles weren't handled so they were just disabled, now you can use them!
- **Hosts save player data in their save game**: The host persists things like ammo, dark eco, health, enabled secrets now for each player, so rejoining won't reset it
## Changed
- **New Player List**: The player list opens by pressing **SELECT**, has a shortcut to **Multiplayer Options** by pressing **Start** while having the list open
- **Blackout when teleporting**: Now you get a blackout screen when being teleported to like a cutscene trigger for example, you won't see the level de-load around you at least
## Fixed
- **Keira race traffic density**: Had a false override over the race traffic density during that race, now you shouldn't have to fight over tons of pedestrians while racing
- **Peacemaker**: You don't need to hold out a gun for you to see other players shoot their peacemaker
- **Find pumping station patrol**: The battle now starts correctly every time, no need to rehost the session
- **Protect Kor and Kid**: The transport spawn should be more consistent now for clients (sometimes the spawn was just missed)
- **Dead players won't block the airlock anymore**: Also if you're alone in a session you won't get the generic Waiting for others... prompt either
- **Dark Jak icon is back**: Previously it was showing the regular Jak hud texture when you had the meter filled up or turned into Dark Jak

