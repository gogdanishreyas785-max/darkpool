# DARKPOOL Quickstart

For the maintained, full guide—including rules, all commands, private-LAN TLS setup, token handling, communications internals, and troubleshooting—see the [User, Game, and Operator Guide](user-guide.md).

## First game on one Linux computer

From the repository root:

~~~bash
bash scripts/setup_local.sh
~~~

Open Terminal 1 and keep the server running:

~~~bash
bash scripts/run_local_server.sh
~~~

In a separate terminal, start the moderator:

~~~bash
bash scripts/run_local_client.sh moderator
~~~

Start one player client per seat (five player clients for the default five-player match):

~~~bash
bash scripts/run_local_client.sh player
~~~

In the moderator terminal:

~~~text
create 5
~~~

The local server launcher stores protected match tokens in the private token store. In each player terminal, you can join without copying the token:

~~~text
join
~~~

You can also specify the match explicitly with `join 1`. If you run the binaries directly instead of using the launcher scripts, pass the match join token explicitly: `join 1 <MATCH_JOIN_TOKEN>`.

Wait until all five players have joined, then in the moderator terminal:

~~~text
start
event -1
~~~

Replace -1 with any event impact from -2 through +2. Discussion, Trading, Clue, transition, and Voting progress on server-owned timers; the moderator chooses the next round's event if the match continues.

For an open lobby without a match join token, use **create 5 open**. This disables token-based admission and should only be used in a trusted environment.

## Multiple computers

Use the [TLS 1.3 private-LAN setup in the user guide](user-guide.md#3-option-b-multiple-computers-over-a-private-lan). Create the protected match in the moderator client before launching player clients; each player launcher then synchronizes protected match-token files over SSH/SCP using the server SSH account saved during setup. If no token was present during launch, relaunch the player script after the moderator creates the match or use the manually shared match token. Never share the moderator authentication token with players.
