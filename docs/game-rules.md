# DARKPOOL — game rules, five-player baseline, and approved variable-size rules

**Operator guide:** See [the User, Game, and Operator Guide](user-guide.md) for setup, commands, current runtime behavior, and troubleshooting. This file is the detailed game-mechanics reference.

**Implementation note:** The moderator currently supplies a numeric event impact with the `event` command. A public narrative event deck and automatic production event selection are not implemented.

**Source status:** Draft 3 directly specifies the five-player baseline and round sequence. The user's S1 follow-up approved the listed baseline mechanics. A later explicit follow-up approves variable-size matches and role scaling below. Scoring remains open.

## Roles and scaling

- **Direct baseline requirement:** five active players: one Insider, one Investigator, three Employees. **Approved for S1:** the moderator is a separate non-player.
- Insider damages company performance and avoids identification; receives one confidential event hint; may trade like others.
- Investigator helps Employees identify the Insider; may request one limited clue per match.
- Employees discuss, trade and vote to protect the company and identify the Insider.
- **Approved variable-size rule:** N is configured active-player capacity (3–64); the moderator is excluded. Five remains the default. Insiders=`ceil(N/8)`, Investigators=`ceil(N/16)`, Employees=`N-Insiders-Investigators`. Use integer arithmetic: `1+(N-1)/8` and `1+(N-1)/16` for valid N. The server builds exactly those role counts and shuffles the role list before assigning server-owned seats; clients cannot choose roles.
- Role examples: N=3 → 1/1/1; N=5 → 1/1/3; N=8 → 1/1/6; N=9 → 2/1/6; N=16 → 2/1/13; N=24 → 3/2/19; N=32 → 4/2/26; N=64 → 8/4/52.
- **Approved S1 moderator boundary:** separate non-player, not one of the five; may create, start, close and inspect operational status. Secret audit access and chat/audit retention remain separate decisions.

## Three rounds

1. The moderator supplies the round's numeric impact. The Insider can retrieve the exact impact privately with `hint`; the current implementation does not publish a separate narrative event description.
2. Public discussion and authorized private messages.
3. Each eligible active player submits one BUY, HOLD, or SELL.
4. Server validates and resolves trades; updates cash/shares, company health and price.
5. Players discuss evidence; Investigator may use the once-per-match clue.
6. Eligible players vote; server resolves result and any elimination.
7. The match ends immediately on the approved health-zero or Insider-elimination outcome; otherwise evaluate the Insider win after round three's vote.

Draft 3 does not define deadlines. Under the approved Stage 3.2 rules, Clue is timed for 30 seconds following market resolution, automatically followed by a distinct 10-second transition interval, which automatically opens Voting with its 60-second deadline.

## S1-approved share supply and trading

The PDF gives no starting balances/supply model. N is the match's configured active-player count; all configured seats must join before start, so no empty seat receives shares.

- Start each player with 100 fictional credits and 10 shares; total issued supply is 20N shares. The match-local reserve starts with 10N; initial player holdings total 10N. Five seats: total 100/reserve 50. Sixty-four seats: total 1,280/reserve 640.
- Reserve is finite and supply conserved. BUY transfers one share from reserve to player and credits to reserve. SELL transfers one share from player to reserve and pays the player. Both settle at the share price frozen at round open. HOLD has no effect.
- Start company health 5, bounds 0–10; start price 10, bounds 1–100.
- **Approved S1 credit-pool model:** credits are player counters, not a globally conserved supply. BUY debits the player; SELL credits the player at opening P. There is no cash-reserve sufficiency check or shared treasury constraint; cap each balance at the provisional S1 implementation limit of 1,000,000 credits (uint32_t); reject any trade that would exceed it. The user directed this safe cap as an implementation detail, not as a new gameplay rule. The non-conserved credit model is approved for S1 but is not specified by the PDF.
- BUY requires enough opening cash and reserve inventory; SELL requires an owned share. Invalid action is rejected with no balance change and consumes the one action slot. Missing action is HOLD. A player may submit only once per round.
- At round close, freeze opening reserve. All eligible SELLs add shares only after BUY allocation; they cannot fund same-round buys. If BUY demand exceeds opening reserve, accept BUYs in rotating seat order starting at ((round number - 1) mod N), reject remaining buys without mutation. Only executed trades count toward demand.

These values and mechanics are approved for S1 by the user's follow-up; they are not PDF requirements.

## S1-approved deterministic business and market rules

Draft 3 requires business events and updates to health/price but supplies no impacts or formulas.

Each round stores a signed impact e in {-2, -1, 0, +1, +2}. In the current implementation, the moderator supplies this number through `event <impact>`; this starts Discussion and the coordinator starts its timer. The Insider can retrieve the exact signed impact through the private `hint` command. A public narrative event-description deck and automated production event selection are not implemented; event text/table and random-seed selection remain open. Tests inject an exact event sequence. Identical initial state, event sequence, accepted actions and votes must produce identical results.

At round start freeze opening health H, price P, and reserve. At trading close, validate submissions, resolve BUYs against opening inventory in rotating seat order, then settle accepted trades at P. Let B and S be executed BUY and SELL counts; sign(x) is -1 for x<0, 0 for x=0, and +1 for x>0.

- Health_next = clamp(H + e + sign(B-S), 0, 10). The event changes health by e; net demand changes health by one in its direction. This lets net selling, including the Insider's action, damage company performance.
- Price_next = clamp(P + 2e + sign(B-S), 1, 100). The event has twice its health weight on price; net demand shifts it by one credit. Equal BUY/SELL counts add zero demand effect.
- Update order: freeze opening H, P, and reserve; resolve executed trades and B/S; settle at P; calculate health and price from opening values; check zero-health victory; only then continue to clue/vote.

Bounds are inclusive. The formulas and update order are approved for S1. Production event text/table and random-seed selection remain open and outside this engine milestone.

### Worked example: ordinary five-player round

At match start, N=5 gives total supply 100 shares: 50 player-held and 50 in reserve. Each player has 100 credits; H=5 and P=10. In round 1, the fixed test event is e=-1. Two players submit valid BUY, one submits valid SELL, and two HOLD. All are funded/eligible; reserve is sufficient.

- Resolve at the opening price P=10: buyers each pay 10 credits and gain one share; seller receives 10 credits and loses one share. Reserve changes 50-2+1=49. Player shares change from 50 to 51, so 51+49=100 remains conserved. Player cash totals 490 after these trades; credits are not globally conserved under the proposed credit-pool model because SELL credits are issued by the match's credit pool.
- Executed B=2, S=1, so sign(B-S)=+1. H'=clamp(5-1+1,0,10)=5. P'=clamp(10+2(-1)+1,1,100)=9.
- The market result is committed before the proposed health-zero check; because H'=5, the match proceeds to the clue/vote stages.

### Worked example: reserve exhaustion boundary (synthetic test fixture)

This is a valid conserved test fixture, but it is not reachable from the recommended starting state during ordinary three-round play. For N=5, reserve starts at 50 and there are at most 5 BUYs in each of 3 rounds; even with no SELLs, at least 35 reserve shares remain. For N=64 the corresponding lower bound is 448. This makes natural exhaustion impossible under one action/player/round and a three-round match.

To test the finite-reserve rejection boundary without changing those rules, initialize a test-only conserved N=5 state with reserve=1 and player holdings totaling 99 (total supply remains 100); give two active players enough cash to BUY. In round 1, seat 0 and seat 1 both submit BUY; rotating priority starts at seat 0. Seat 0's BUY settles at opening P; reserve becomes zero and player holdings become 100. Seat 1's BUY is rejected for insufficient reserve, with no balance mutation. The rejected order is not counted in B; only executed trades feed the formulas. This test proves the boundary behavior, not that ordinary play reaches it.

The examples illustrate the S1-approved formulas and reserve boundary fixture; reserve exhaustion remains synthetic, not ordinary gameplay.

## S1-approved clue and Stage 3.2 timed progression

Draft 3 only says one limited clue per match. A randomly revealed player's trade may be weak or redundant if trades are visible.

**Approved clue:** each active Investigator may use one clue per match after trade resolution and before that round's vote. The Investigator selects one other active player and privately receives a binary result: “target is an Insider” or “target is not an Insider.” A same-target retry by the same Investigator returns the same result without consuming another use; a different target after a successful clue is rejected. Clues and clue-use state are independent per Investigator. A target eliminated later does not invalidate its historical clue.

**Stage 3.2 Approved Timing Sequence:**
- **Clue (30 seconds):** Entered automatically after authoritative market resolution with a 30-second monotonic deadline. Emits `phase_started` and an immediate 30-second countdown warning (`seconds_remaining: 30`) at phase entry, followed by a 10-second countdown warning (`seconds_remaining: 10`) at ~10 seconds remaining. Active Investigators may submit their private clue request throughout this 30-second window. Voting is not open and votes are not accepted during Clue.
- **Transition Interval (10 seconds):** When the 30-second Clue deadline expires, the server automatically starts the distinct 10-second transition interval. Emits a `transition_interval` announcement reporting 10 seconds remaining before the upcoming `Voting` phase. Clue requests are rejected once the transition interval begins. Voting commands are not accepted before the transition interval ends.
- **Voting (60 seconds):** When the 10-second transition interval expires, the server automatically opens Voting with its 60-second deadline and broadcasts the phase-start event with a public market snapshot. Votes resolve automatically upon Voting deadline expiry.
- **Pause/Resume/Abort:** Pausing freezes remaining time during Clue or the transition interval; resuming restores the exact saved duration. Aborting permanently cancels all subsequent scheduled transitions and market/vote resolutions.

## S1-approved voting

PDF does not define eligibility, abstentions, missing votes, ties, threshold or consequences.

- All active players at vote open, including every Insider and Investigator, may cast one secret vote for another active player or abstain. No self-vote. Eliminated players cannot vote or be targets.
- Missing vote at deadline counts as abstention.
- Let E be active eligible voters at vote open. Eliminate a target only if they receive strictly more than E/2 votes. Abstentions remain in denominator. Tie/no strict majority means no elimination.
- Eliminated Employee cannot trade, clue, vote or send game chat later; their cash/shares remain. They may receive public phase/results. No second vote that round.

These edge rules are approved for the S1 prototype by the user's follow-up.

## S1-approved victory

Draft 3 requires final evaluation but gives no victory rules.

- If health reaches zero at market resolution, the Insider team wins immediately before clue/vote; calculate/report final price.
- If a vote eliminates an Insider while health is positive, that player is eliminated. Employees win immediately only if no active Insiders remain.
- Eliminating an Employee does not end the game.
- If round three's vote resolves with health above zero and at least one Insider active, the Insider team wins immediately at final evaluation.
- No draw under this proposal. Immediate outcomes avoid conflicts: the match ends on zero health or Insider elimination.

These outcomes and precedence are approved for S1 by the user's follow-up; they are not rules supplied by the PDF.

## Stage 4 — Advanced Market Mechanics, Quantity Trading, and Research/Scouting

The following mechanics were introduced in Stage 4:

### 1. Quantity-Based Trading
- **Syntax**: `buy [quantity]`, `sell [quantity]`, `trade <buy|sell|hold> [quantity]`.
- **Default Compatibility**: If `quantity` is omitted, the order defaults to 1 share, preserving complete compatibility with single-share commands.
- **Validation**:
  - Quantity must be a strictly positive integer ($q \ge 1$). Zero, negative, overflowing, or malformed quantities are rejected without state mutation.
  - BUY requires opening-price cash: $player.credits \ge q \times P$.
  - SELL requires owned inventory: $player.shares \ge q$.
  - SELL requires sufficient owned shares and must not raise the player's balance above the per-player credit cap of 1,000,000 credits.
- **Finite Reserve and All-or-Nothing Allocation**:
  - Reserve shares are strictly finite ($10N$ initial for $N$ players; total supply $20N$).
  - Allocation resolves in deterministic rotating seat order starting at $((round - 1) \bmod N)$. *(Provenance Note: Rotating seat priority is a canonical S1-approved rule established in S0/S1 under DEC-04 and lines 36 & 46. Stage 4 preserves this canonical ordering for multi-quantity orders with all-or-nothing allocation).*
  - If remaining reserve inventory $\ge q$, the order is filled completely (`Executed`).
  - If remaining reserve inventory $< q$, the order is rejected entirely (`RejectedInsufficientReserve`). Partial fills and limit order books are strictly forbidden.
- **Supply Conservation**: Every transaction conserves total shares: $\sum player.shares + reserve = total\_supply = 20N$. Shares are never created or destroyed.
- **Missing Orders**: Default to `HOLD` (0 shares, no cash change).

### 2. Research / Scouting Action
- **Cost**: Exactly 10 credits debited from the requesting active player.
- **Usage Limit**: At most once per player per match.
- **Timing Window**: Available strictly during the pre-trading information window (`Phase::Discussion`), after the business event is announced and before trading opens. Rejected outside this phase without charge.
- **Result & Privacy**: Returns a private, coarse direction signal:
  - `"Positive"` if $e > 0$
  - `"Neutral"` if $e = 0$
  - `"Negative"` if $e < 0$
  - The exact hidden numeric impact $e \in \{-2, -1, 0, 1, 2\}$ is never leaked.
- **Idempotency**: Repeated requests by the same player return the cached direction, report `already_used: true`, and do NOT charge credits again.
- **Audience Isolation**: The report is sent strictly to the requesting session and never appears in public broadcasts, snapshots, or other players' queues.

### 3. Public vs. Private Market Information
- **Authoritative Public Fields**: `phase`, `round`, `health` (0–10), `price` (1–100), `sentiment` (-10..+10), `volatility` (0..20, grounded in gross volume $G = B + S$), `liquidity` (0..100%), `total_supply`, `reserve`, `shares_held_by_players`, `outcome`, `state_version`.
- **Confidential Private Fields**: Individual cash balances, individual share holdings, private roles, Investigator clue results, and player research reports remain strictly confidential to each authenticated player.

### 4. Advanced Deterministic Price Model
- Implements deterministic pricing combining business event effects, net trade demand direction and volume, volatility magnification, company health deviations, market sentiment, and low-reserve scarcity surcharges, strictly computed using mathematical integer floor division via safe helper `Match::floor_div`.
- Numerical coefficients are maintained centrally in `struct MarketModelCoefficients` and documented in `docs/market-model-proposal.md`.
- Baseline coefficients (`approved_baseline()`) remain active by default for complete canonical compatibility; proposed candidate coefficients (`candidate_proposal()`) are pending user review.

## Proposed scoring — separate decision still open

The PDF gives no individual scoring or tie-break system. Recommended default: the only score is the winning side; cash and shares affect play but do not create individual points, leaderboard, or victory tie-break. A final player summary may show the approved public result and that player's own balances, but not private role/clue data. Approve or replace this scoring/summary policy.

No real money or excluded market feature is introduced.
