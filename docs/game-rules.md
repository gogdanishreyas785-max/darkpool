# Proposed game rules

**Status:** Roles and broad loop are from Draft 3. Every proposed default below fills a gap; it is not a PDF requirement and needs approval before implementation.

## Roles and scope

- **Insider:** Seeks to damage company performance and avoid identification; receives one confidential event hint; may trade like others.
- **Investigator:** Helps Employees identify Insider; may request one limited clue per match.
- **Employee:** Discusses, trades, and votes to protect company and identify Insider.
- **Baseline:** Five active players: one Insider, one Investigator, three Employees. The PDF also shows a moderator but asks that status be clarified.
- **Proposed default — approval required:** Moderator is a separate non-player operator, not one of the five seats. May create/start/close matches and inspect operational status. No secret visibility through player output; any privileged audit access is separately permissioned.
- **Proposed default — approval required:** At larger sizes retain one Insider and one Investigator; all other seats are Employees. This extension is not specified and may affect balance.

## Three-round sequence

1. Server announces round, publishes a business event, and privately sends Insider hint.
2. Public discussion; authorized private messages may be sent.
3. Every eligible active player submits one BUY, HOLD, or SELL.
4. Server validates actions and updates cash, shares, company health, and price.
5. Players discuss evidence; Investigator may use the once-per-match clue.
6. Eligible players vote; server records the result under approved voting rules.
7. Repeat through three rounds; evaluate win condition and issue permitted final summary.

The PDF does not define phase deadlines or exact clue timing. Appendix A places clue before vote; the above ordering is proposed, pending approval.

## Proposed economy (approval required)

PDF mentions one company share/instrument and updates to cash, shares, health, and price, but gives no starting balances or formula. Suggested fictional units only, never real money:

- Start cash 100 credits/player; start shares 10/player.
- Fixed total supply 640 shares/match; unissued shares are held by a match-local reserve.
- Start health 5 on a 0–10 scale; start share price 10 credits.
- BUY/SELL trades one share with the reserve at round-opening price. HOLD is no-op.
- BUY requires sufficient cash and reserve inventory; SELL requires a held share. Invalid/late/duplicate action does not mutate state. Missing action at deadline becomes HOLD.
- Each business event has impact -1, 0, or +1. Apply to health and price. After trades, price is clamp(opening price + event impact + sign(valid BUY count - valid SELL count), 1, 100); sign is -1, 0, or +1. A demand tie has no demand adjustment.
- Reserve collects buyers' credits and pays sellers.

This avoids order books/auctions but is a new market model. Approve or replace before S1.

## Business events and Insider hint

PDF requires a business event and confidential hint, not their content or effects.

**Proposed default — approval required:** Pick each event from a fixed server-side table using a server-generated match seed. Publish event text to all; privately tell Insider its true health impact before trades. Apply impact during resolution. Keep seed in audit, not player output. No external feed. Approve event table, randomness/replay policy, and whether impact is later revealed.

## Investigator clue

PDF permits one limited clue per match but does not define its content, timing, audience, or consequence.

**Proposed default — approval required:** Once per match, after trade resolution and before vote, Investigator may request a clue privately revealing one randomly selected other player's resolved BUY/HOLD/SELL. No additional consequence. A retry must not consume a second clue. Approve content and availability window.

## Chat

PDF requires public chat and authorized private messages but does not define recipient policy, retention, or post-elimination access.

**Proposed default — approval required:** Public messages go only to active members of that match; whisper is one-to-one among active members. No cross-match recipients. Do not persist chat content by default; security metadata goes to a separate access-controlled audit log. Approve retention and moderator access.

## Voting and consequences

The PDF does not state eligible voters, deadline, ties, threshold, elimination, or consequences.

**Proposed defaults — approval required:** All active players may vote for another active player, not self. One vote each; missing vote is abstention. Eliminate only if a target receives strictly more than half of non-abstaining votes; a tie or no majority eliminates nobody. Eliminated players cannot trade, clue, vote, or send game chat in later rounds, but may receive public results. Continue schedule to round three. Keep vote identities secret; reveal only approved outcome.

## Victory and scoring

PDF requires final evaluation after three rounds but supplies no victory or scoring formula.

**Proposed default — approval required:** Employees win if Insider is eliminated and health is above zero; otherwise Insider wins. This gives an exclusive team result and uses both identification and company health. No individual economic score changes team victory. Final player summary contains permitted public company state and outcome, not secret role/clue details.

Whether zero health ends immediately, what happens after early elimination, scoring, and simultaneous conditions need approval. No default above should be implemented until reviewed.
