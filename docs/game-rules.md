# Proposed game rules

**Source status:** Draft 3 directly specifies the baseline sequence and roles; detailed mechanics below were S0 proposals. The user's S1 follow-up explicitly approves the S1 gameplay rules listed there. Proposals outside that follow-up (including larger-match scaling and scoring) remain open.

## Roles and scaling

- **Direct baseline requirement:** five active players: one Insider, one Investigator, three Employees. The moderator is shown separately in the example; status needs clarification.
- Insider damages company performance and avoids identification; receives one confidential event hint; may trade like others.
- Investigator helps Employees identify the Insider; may request one limited clue per match.
- Employees discuss, trade and vote to protect the company and identify the Insider.
- **Scaling proposal — approval required:** retain one Insider and one Investigator at all sizes, all remaining seats Employees. This preserves the singular hidden target and baseline rules; at 64 players the lone Insider is harder to identify and one Investigator may be weak. This is a proposal, not an assumed rule.
- **Moderator proposal — approval required:** separate non-player, not one of the five; can create/start/close and inspect operational status. No secret visibility through player output; privileged audit access needs a separate policy.

## Three rounds

1. Server announces round and business event; privately sends Insider hint.
2. Public discussion and authorized private messages.
3. Each eligible active player submits one BUY, HOLD, or SELL.
4. Server validates and resolves trades; updates cash/shares, company health and price.
5. Players discuss evidence; Investigator may use the once-per-match clue.
6. Eligible players vote; server resolves result and any elimination.
7. Continue until a proposed immediate victory occurs or round three's vote resolves; issue permitted final summary.

Draft 3 does not define deadlines. Appendix A puts clue use before the vote. Exact timing and clue window need approval.

## Proposed share supply and trading — approval required

The PDF gives no starting balances/supply model. Let N be the number of active player seats actually filled when the match starts, from 5–64. Freeze that roster at match start; do not issue shares for empty configured seats.

- Start each player with 100 fictional credits and 10 shares; total issued supply is 20N shares. The match-local reserve starts with 10N; initial player holdings total 10N. Five seats: total 100/reserve 50. Sixty-four seats: total 1,280/reserve 640.
- Reserve is finite and supply conserved. BUY transfers one share from reserve to player and credits to reserve. SELL transfers one share from player to reserve and pays the player. Both settle at the share price frozen at round open. HOLD has no effect.
- Start company health 5, bounds 0–10; start price 10, bounds 1–100.
- **Credit-pool proposal — approval required:** credits are player counters, not a globally conserved supply. BUY debits the player; SELL credits the player at opening P. There is no cash-reserve sufficiency check or shared treasury constraint; cap each balance at the provisional S1 implementation limit of 1,000,000 credits (uint32_t); reject any trade that would exceed it. This cap is an implementation detail selected under the user's S1 instruction, not a new gameplay rule, and may be revisited. This avoids an undefined seller funding source but is a gameplay/economy choice, not specified by the PDF.
- BUY requires enough opening cash and reserve inventory; SELL requires an owned share. Invalid action is rejected with no balance change and consumes the one action slot. Missing action is HOLD. A player may submit only once per round.
- At round close, freeze opening reserve. All eligible SELLs add shares only after BUY allocation; they cannot fund same-round buys. If BUY demand exceeds opening reserve, accept BUYs in rotating seat order starting at ((round number - 1) mod N), reject remaining buys without mutation. Only executed trades count toward demand.

These values and mechanics are proposed defaults, not PDF requirements.

## Proposed deterministic business and market rules — approval required

Draft 3 requires business events and updates to health/price but supplies no impacts or formulas.

Each event has signed impact e in {-2, -1, 0, +1, +2}. **Proposed event/hint mapping — approval required:** publish the event description to all players and privately tell the Insider its true signed health impact before trading; do not reveal the numeric impact to other players during the match. Production may select from an approved fixed table using a server-generated seed recorded in restricted audit. Tests inject an exact event sequence. Identical initial state, event sequence, accepted actions and votes must produce identical results.

At round start freeze opening health H, price P, and reserve. At trading close, validate submissions, resolve BUYs against opening inventory in rotating seat order, then settle accepted trades at P. Let B and S be executed BUY and SELL counts; sign(x) is -1 for x<0, 0 for x=0, and +1 for x>0.

- Health_next = clamp(H + e + sign(B-S), 0, 10). The event changes health by e; net demand changes health by one in its direction. This lets net selling, including the Insider's action, damage company performance.
- Price_next = clamp(P + 2e + sign(B-S), 1, 100). The event has twice its health weight on price; net demand shifts it by one credit. Equal BUY/SELL counts add zero demand effect.
- Update order: freeze opening H, P, and reserve; resolve executed trades and B/S; settle at P; calculate health and price from opening values; check zero-health victory; only then continue to clue/vote.

Bounds are inclusive. Formulas, impact range, event table and random-seed policy all require approval.

### Worked example: ordinary five-player round (proposal illustration)

At match start, N=5 gives total supply 100 shares: 50 player-held and 50 in reserve. Each player has 100 credits; H=5 and P=10. In round 1, the fixed test event is e=-1. Two players submit valid BUY, one submits valid SELL, and two HOLD. All are funded/eligible; reserve is sufficient.

- Resolve at the opening price P=10: buyers each pay 10 credits and gain one share; seller receives 10 credits and loses one share. Reserve changes 50-2+1=49. Player shares change from 50 to 51, so 51+49=100 remains conserved. Player cash totals 490 after these trades; credits are not globally conserved under the proposed credit-pool model because SELL credits are issued by the match's credit pool.
- Executed B=2, S=1, so sign(B-S)=+1. H'=clamp(5-1+1,0,10)=5. P'=clamp(10+2(-1)+1,1,100)=9.
- The market result is committed before the proposed health-zero check; because H'=5, the match proceeds to the clue/vote stages.

### Worked example: reserve exhaustion boundary (proposed synthetic test)

This is a valid conserved test fixture, but it is not reachable from the recommended starting state during ordinary three-round play. For N=5, reserve starts at 50 and there are at most 5 BUYs in each of 3 rounds; even with no SELLs, at least 35 reserve shares remain. For N=64 the corresponding lower bound is 448. This makes natural exhaustion impossible under one action/player/round and a three-round match.

To test the finite-reserve rejection boundary without changing those rules, initialize a test-only conserved N=5 state with reserve=1 and player holdings totaling 99 (total supply remains 100); give two active players enough cash to BUY. In round 1, seat 0 and seat 1 both submit BUY; rotating priority starts at seat 0. Seat 0's BUY settles at opening P; reserve becomes zero and player holdings become 100. Seat 1's BUY is rejected for insufficient reserve, with no balance mutation. The rejected order is not counted in B; only executed trades feed the formulas. This test proves the boundary behavior, not that ordinary play reaches it.

All examples illustrate pending proposals and do not approve them.

## Proposed clue — approval required

Draft 3 only says one limited clue per match. A randomly revealed player's trade may be weak or redundant if trades are visible.

**Recommended alternative:** once per match, after trade resolution and before that round's vote, Investigator selects one other active player and privately receives a binary result: “target is the Insider” or “target is not the Insider.” This is simple, testable and meaningful, but powerful; one use per match limits it. Retry cannot consume another use. Approve or replace before implementation.

## Proposed voting — approval required

PDF does not define eligibility, abstentions, missing votes, ties, threshold or consequences.

- All active players at vote open, including Insider and Investigator, may cast one secret vote for another active player or abstain. No self-vote. Eliminated players cannot vote or be targets.
- Missing vote at deadline counts as abstention.
- Let E be active eligible voters at vote open. Eliminate a target only if they receive strictly more than E/2 votes. Abstentions remain in denominator. Tie/no strict majority means no elimination.
- Eliminated Employee cannot trade, clue, vote or send game chat later; their cash/shares remain. They may receive public phase/results. No second vote that round.

Every bullet is a proposed default requiring approval.

## Proposed victory — approval required

Draft 3 requires final evaluation but gives no victory rules.

- If health reaches zero at market resolution, Insider wins immediately before clue/vote; calculate/report final price.
- If a vote eliminates Insider while health is above zero, Employees win immediately after that vote.
- Eliminating an Employee does not end the game.
- If round three's vote resolves without eliminating Insider and health is above zero, Insider wins immediately at final evaluation.
- No draw under this proposal. Immediate outcomes avoid conflicts: the match ends on zero health or Insider elimination.

These outcomes and precedence are proposed, not specified by the PDF.

## Proposed scoring — approval required

The PDF gives no individual scoring or tie-break system. Recommended default: the only score is the winning side; cash and shares affect play but do not create individual points, leaderboard, or victory tie-break. A final player summary may show the approved public result and that player's own balances, but not private role/clue data. Approve or replace this scoring/summary policy.

No real money or excluded market feature is introduced.
