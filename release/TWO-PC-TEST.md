# The first test on two PCs: a checklist

Everything so far was tested with two copies of the game on one PC. This is what to try, and what to note, the first time
two people play on two PCs. Set-up steps are in [README-PLAYERS.md](README-PLAYERS.md); this page assumes both copies are
made and the overlay VPN works (each PC can ping the other's VPN address).

Note the time when each step starts: the logs are matched to it afterwards.

## Before

- [ ] Both: the same mod build (compare `Get-FileHash <CopyDir>\x64\bink2w64.dll` on both PCs) and the same password.
- [ ] Both: the game build is 3095001 (`setup-copy.ps1` refuses another).
- [ ] Host: Windows asked to allow the game through its firewall, and "private networks" was allowed (or the UDP port is
      open). Note the address the host's log gives (`a client ... joins with coop_host=...`).
- [ ] Note each side's connection: wired or Wi-Fi, and roughly how far apart the players are.

## In the session (about 30 minutes)

1. [ ] **Join.** The host is in the world first; then the client starts. Note how long the client takes from start to
       the world, and whether it showed a message box. (The save is about 2.3 MB; the client's log shows its progress.)
2. [ ] **See each other.** Each player walks and runs around the other: does the other's character walk (not slide or
       jump), and does it stay where the other player is?
3. [ ] **A fight.** Find a group of orcs together: the client's hits hurt them (on both screens), they attack the client,
       the client takes damage, an orc killed by either dies on both screens.
4. [ ] **Counters and arrows.** The client counters an attack and shoots an orc with the bow.
5. [ ] **Fast travel.** The host fast-travels: the client is taken along within a few seconds.
6. [ ] **The host dies.** The client should keep playing where it is until the host walks again after its death screens,
       then be taken to the host.
7. [ ] **The client dies.** It respawns and is taken back to the host a few seconds later.
8. [ ] **A captain.** Meet or fight a captain: is he the same captain on both screens?
9. [ ] **Play on** for the rest of the 30 minutes and note anything odd, with the time.
10. [ ] **Quit and rejoin.** The client quits and starts again: it joins the same session.

## After

- [ ] Both: `<CopyDir>\profile\sowguard.log`. Replace your Windows user name in it (for example with `USER`) before
      sending; say which side it is from.
- [ ] The notes with the times, and for each step: worked, did not, or not tried.
- [ ] Anything that crashed or froze: which side, what was happening, the time.
