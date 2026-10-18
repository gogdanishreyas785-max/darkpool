# DARKPOOL Stage 4.3 — Reproducible Market-Balance and Sensitivity Analysis

> [!WARNING]
> **GOVERNANCE STATUS: UNAPPROVED CANDIDATE PROPOSAL**
> This document reports quantitative measurements and sensitivity analysis for the candidate market-pricing model.
> **The legacy-approved market coefficients remain active by default across all ordinary gameplay, coordinator creation, and network matches.**
> The candidate pricing weights ($w_e=2, w_d=1, w_v=1, w_{\text{vol}}=1, w_h=1, w_s=1, w_l=2$), directional volatility magnification, and positive low-reserve scarcity surcharge remain **unapproved candidate design proposals awaiting explicit user review**.
> Ordinary matches strictly use `MarketModelCoefficients::approved_baseline()`.

---

## 1. Mathematical Formulas Implemented by Current Engine

The actual C++ engine (`src/game_engine.cpp`) implements the following deterministic formulas for market clearing and price adjustment:

### 1.1 Trading Quantities and Indicators

- **Net Trade Demand**:
  $$D = B - S$$
  $$\text{sign}(D) = \begin{cases} +1 & \text{if } D > 0 \\ 0 & \text{if } D = 0 \\ -1 & \text{if } D < 0 \end{cases}$$
- **Gross Executed Volume**:
  $$G = B + S$$

- **Company Health Transition**:
  $$H' = \text{clamp}(H + e + \text{sign}(D), 0, 10)$$
  where $e \in \{-2, -1, 0, 1, 2\}$ is the public business event impact.

- **Reserve Liquidity Indicator ($\mathcal{L}$)**:
  $$\mathcal{L} = \text{clamp}\left(\left\lfloor \frac{\text{Reserve} \times 100}{\text{Total Supply}} \right\rfloor, 0, 100\right)$$
  For standard matches ($N=5$, $\text{Total Supply} = 100$), each reserve share equals exactly 1% liquidity.

- **Sentiment Indicator ($\mathcal{S}$)**:
  $$\Delta \mathcal{S} = \begin{cases} \text{sign}(D) & \text{if } |D| = 1 \\ \text{clamp}\left(\lfloor \frac{D}{2} \rfloor, -3, 3\right) & \text{if } |D| > 1 \\ 0 & \text{if } D = 0 \end{cases}$$
  $$\mathcal{S}' = \text{clamp}(\mathcal{S} + \Delta \mathcal{S}, -10, 10)$$

- **Volatility Indicator ($\mathcal{V}$)**:
  $$\mathcal{V} = \text{clamp}(G + 2 \cdot |e|, 0, 20)$$
  Grounding volatility in gross volume $G$ guarantees that all executed buy and sell volume is captured even when net demand is zero.

### 1.2 Mathematical Floor Division

All division in integer terms uses mathematical floor division $\lfloor a / b \rfloor$ via `Match::floor_div(a, b)`:
$$\text{floor\_div}(a, b) = \begin{cases} a / b & \text{if } (a \ge 0 \text{ and } b > 0) \text{ or } (a \le 0 \text{ and } b < 0) \text{ or } (a \% b == 0) \\ (a / b) - 1 & \text{otherwise} \end{cases}$$
This ensures that negative odd quantities round toward negative infinity (e.g., $\lfloor -1/2 \rfloor = -1$, $\lfloor -3/2 \rfloor = -2$) rather than truncating toward zero.

### 1.3 Price Adjustment Equation

$$\Delta P = \text{term}_e + \text{term}_d + \text{term}_v + \text{term}_{\text{vol}} + \text{term}_h + \text{term}_s + \text{term}_l$$

where:
- $\text{term}_e = w_e \cdot e$
- $\text{term}_d = w_d \cdot \text{sign}(D)$
- $\text{term}_v = \begin{cases} w_v \cdot \left(\text{sign}(D) \cdot \left\lfloor \frac{|D| - 1}{2} \right\rfloor \right) & \text{if } |D| > 1 \\ 0 & \text{otherwise} \end{cases}$
- $\text{term}_{\text{vol}} = \begin{cases} w_{\text{vol}} \cdot \left(\text{dir}_{\text{market}} \cdot \left\lfloor \frac{\mathcal{V}}{4} \right\rfloor \right) & \text{if } \text{dir}_{\text{market}} \ne 0 \\ 0 & \text{otherwise} \end{cases}$
- $\text{term}_h = w_h \cdot (H' - 5)$
- $\text{term}_s = \begin{cases} w_s \cdot \left\lfloor \frac{\mathcal{S}'}{2} \right\rfloor & \text{if } \mathcal{S}' \ne 0 \\ 0 & \text{otherwise} \end{cases}$
- $\text{term}_l = \begin{cases} w_l & \text{if } D > 0 \text{ and } \mathcal{L} < 20\% \\ 0 & \text{otherwise} \end{cases}$

Market direction is established by net demand, falling back to business event impact when trading is neutral:
$$\text{dir}_{\text{market}} = \begin{cases} \text{sign}(D) & \text{if } D \ne 0 \\ \text{sign}(e) & \text{if } D = 0 \text{ and } e \ne 0 \\ 0 & \text{if } D = 0 \text{ and } e = 0 \end{cases}$$

Closing price is bounded by canonical limits:
$$P' = \text{clamp}(P + \Delta P, 1, 100)$$

---

## 2. Active Baseline vs Candidate Coefficients

| Coefficient | Symbol | Parameter Name in C++ | Baseline (Approved Default) | Candidate Proposal | Functional Purpose |
| :--- | :---: | :--- | :---: | :---: | :--- |
| Event Weight | $w_e$ | `event_weight` | **2** | **2** | Fundamental event impact multiplier |
| Net Demand Direction | $w_d$ | `demand_weight` | **1** | **1** | Directional step on non-zero order imbalance |
| Excess Demand Volume | $w_v$ | `volume_weight` | **0** | **1** | Non-linear quantity scaling for $|D| > 1$ |
| Volatility Weight | $w_{\text{vol}}$ | `volatility_weight` | **0** | **1** | Trend magnification based on gross activity |
| Health Weight | $w_h$ | `health_weight` | **0** | **1** | Fundamental valuation anchor around neutral $H=5$ |
| Sentiment Weight | $w_s$ | `sentiment_weight` | **0** | **1** | Momentum feedback from accumulated mood |
| Scarcity Surcharge | $w_l$ | `liquidity_penalty` | **0** | **2** | Supply-squeeze premium when reserve $< 20\%$ |

### Governance and Construction Path
- Ordinary matches created via `Match(roster)` or `MatchCoordinator::handle(session, {"type", "create_match", ...})` default strictly to `MarketModelCoefficients::approved_baseline()`.
- Network clients, CLI commands, and game protocols cannot modify or activate candidate coefficients.
- Candidate coefficients can only be passed programmatically in dedicated test fixtures and analysis harnesses.

---

## 3. Reproduction Instructions

The sensitivity analysis harness is built into the C++ test suite and compiled with warnings-as-errors:

```bash
# 1. Configure in Release mode with warnings-as-errors enabled
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON

# 2. Build the complete project and analysis target
cmake --build build-release --target darkpool_market_sensitivity_analysis -j4

# 3. Run automated invariant verification via CTest
ctest --test-dir build-release -R darkpool_market_sensitivity_analysis --output-on-failure

# 4. Execute the live analysis harness to generate full tabular reports
./build-release/darkpool_market_sensitivity_analysis
```

---

## 4. Single-Round Market-Balance Scenarios

The table below reports measured, side-by-side engine settlement results for the 9 canonical balance scenarios, showing initial state, trading actions, indicators, term breakdown under the candidate formula, and final baseline vs candidate closing prices:

| Scenario | Open $(P, H)$ | Open Res / Total | Event $e$ | Executed Buys / Sells | Net Demand $D$, Gross $G$ | Closing Liq $\mathcal{L}$ | Closing Vol $\mathcal{V}$ | Closing Sent $\mathcal{S}'$ | Close $H'$ | Candidate Term Breakdown $(\text{term}_e, \text{term}_d, \text{term}_v, \text{term}_{\text{vol}}, \text{term}_h, \text{term}_s, \text{term}_l)$ | Candidate Raw $\Delta P$ | Candidate Close $P'$ | Baseline Raw $\Delta P$ | Baseline Close $P'$ |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **1. Neutral market** | $10, 5$ | $50 / 100$ | $0$ | $0 / 0$ | $0, 0$ | $50\%$ | $0$ | $0$ | $5$ | $(0, 0, 0, 0, 0, 0, 0)$ | $0$ | **10** | $0$ | **10** |
| **2. Balanced trading** | $10, 5$ | $50 / 100$ | $0$ | $2 / 2$ | $0, 4$ | $50\%$ | $4$ | $0$ | $5$ | $(0, 0, 0, 0, 0, 0, 0)$ | $0$ | **10** | $0$ | **10** |
| **3A. Mirrored Positive** | $50, 5$ | $50 / 100$ | $+1$ | $2 / 0$ | $+2, 2$ | $48\%$ | $4$ | $+1$ | $7$ | $(+2, +1, 0, +1, +2, 0, 0)$ | $+6$ | **56** | $+3$ | **53** |
| **3B. Mirrored Negative** | $50, 5$ | $50 / 100$ | $-1$ | $0 / 2$ | $-2, 2$ | $52\%$ | $4$ | $-1$ | $3$ | $(-2, -1, 0, -1, -2, -1, 0)$ | $-7$ | **43** | $-3$ | **47** |
| **4. Extreme positive event**| $50, 8$ | $50 / 100$ | $+2$ | $6 / 0$ | $+6, 6$ | $44\%$ | $10$ | $+3$ | $10$ | $(+4, +1, +2, +2, +5, +1, 0)$ | $+15$ | **65** | $+5$ | **55** |
| **5. Extreme negative event**| $50, 2$ | $50 / 100$ | $-2$ | $0 / 6$ | $-6, 6$ | $56\%$ | $10$ | $-3$ | $0$ | $(-4, -1, -2, -2, -5, -2, 0)$ | $-16$ | **34** | $-5$ | **45** |
| **6. High liquidity** | $15, 6$ | $70 / 100$ | $+1$ | $2 / 0$ | $+2, 2$ | $68\%$ | $4$ | $+1$ | $8$ | $(+2, +1, 0, +1, +3, 0, 0)$ | $+7$ | **22** | $+3$ | **18** |
| **7. Low liquidity ($<20\%$)** | $15, 6$ | $21 / 100$ | $+1$ | $2 / 0$ | $+2, 2$ | $19\%$ | $4$ | $+1$ | $8$ | $(+2, +1, 0, +1, +3, 0, \mathbf{+2})$ | $+9$ | **24** | $+3$ | **18** |
| **8A. Upper price clamp** | $95, 8$ | $50 / 100$ | $+2$ | $5 / 0$ | $+5, 5$ | $45\%$ | $9$ | $+2$ | $10$ | $(+4, +1, +2, +2, +5, +1, 0)$ | $+15$ | **100** *(clamped)* | $+5$ | **100** |
| **8B. Lower price clamp** | $5, 2$ | $50 / 100$ | $-2$ | $0 / 5$ | $-5, 5$ | $55\%$ | $9$ | $-3$ | $0$ | $(-4, -1, -2, -2, -5, -2, 0)$ | $-16$ | **1** *(clamped)* | $-5$ | **1** |
| **9A. Health ceiling clamp** | $50, 9$ | $50 / 100$ | $+2$ | $1 / 0$ | $+1, 1$ | $49\%$ | $5$ | $+1$ | $10$ | $(+4, +1, 0, +1, +5, 0, 0)$ | $+11$ | **61** | $+5$ | **55** |
| **9B. Health floor clamp** | $50, 1$ | $50 / 100$ | $-2$ | $0 / 1$ | $-1, 1$ | $51\%$ | $5$ | $-1$ | $0$ | $(-4, -1, 0, -1, -5, -1, 0)$ | $-12$ | **38** | $-5$ | **45** |

### Key Scenario Insights
1. **Zero Price Drift in Neutral Markets**: Both Scenario 1 ($G=0$) and Scenario 2 ($G=4$) produce exactly $\Delta P = 0$. Even though balanced trading drives volatility to $\mathcal{V}=4$, $\text{dir}_{\text{market}} = 0$ ensures that volatility cannot induce artificial upward or downward drift.
2. **Asymmetry in Mirrored Pairs (Scenario 3A vs 3B)**:
   - Mirrored Positive (3A) yields raw $\Delta P = +6$ ($P'=56$).
   - Mirrored Negative (3B) yields raw $\Delta P = -7$ ($P'=43$).
   - **Root Cause**: Mathematical floor division on sentiment. In 3A, $\mathcal{S}=+1 \implies \lfloor 1/2 \rfloor = 0$. In 3B, $\mathcal{S}=-1 \implies \lfloor -1/2 \rfloor = -1$. This 1-point difference is an intended property of mathematical floor division on integers.
3. **Scarcity Activation Boundary (Scenario 6 vs 7)**:
   - Scenario 6 (opening reserve 70 shares, closing reserve 68 shares $\implies 68\%$) produces $\Delta P = +7$ ($P'=22$).
   - Scenario 7 (opening reserve 21 shares, closing reserve 19 shares $\implies 19\% < 20\%$) activates $+w_l = +2$, producing $\Delta P = +9$ ($P'=24$).
   - The engine strictly uses **closing reserve** post-settlement, activating the surcharge exactly when physical depletion occurs.
4. **Price and Health Bound Clamping (Scenarios 8 & 9)**:
   - Large raw deltas ($+15$ and $-16$) cleanly saturate at $100$ and $1$ without integer rollover.
   - Company health strictly clamps at bounds $10$ and $0$, safely terminating the match on zero health.

---

## 5. Systematic Parameter Sensitivity Analysis

A deterministic One-Factor-At-A-Time (OAT) parameter sensitivity analysis evaluated each candidate coefficient across two lower settings and two higher settings, holding all other candidate parameters fixed. In total, 105 distinct parameter configurations were tested across Bullish ($e=+1, D=+2$), Bearish ($e=-1, D=-2$), Neutral ($e=0, D=0$), High-Liquidity ($\mathcal{L}=68\%$), and Low-Liquidity ($\mathcal{L}=19\%$) market conditions.

### 5.1 One-Factor-At-A-Time (OAT) Sensitivity

| Parameter | Explored Range | Candidate Value | Sensitivity in Bullish Case ($P_0=50$) | Sensitivity in Bearish Case ($P_0=50$) | Clamping Saturation Risk | Directional Consistency |
| :--- | :---: | :---: | :--- | :--- | :---: | :---: |
| **$w_e$ (Event Weight)** | $[0, 1, \mathbf{2}, 3, 4]$ | $2$ | $+1$ per unit step ($w_e=0 \to \Delta P=+4$; $w_e=4 \to \Delta P=+8$) | $-1$ per unit step ($w_e=0 \to \Delta P=-5$; $w_e=4 \to \Delta P=-9$) | Moderate at price boundaries | Highly consistent; linear with $e$ |
| **$w_d$ (Demand Direction)**| $[0, \mathbf{1}, 2, 3]$ | $1$ | $+1$ per unit step ($w_d=0 \to \Delta P=+5$; $w_d=3 \to \Delta P=+8$) | $-1$ per unit step ($w_d=0 \to \Delta P=-6$; $w_d=3 \to \Delta P=-9$) | Moderate | Strictly directional; linear with $\text{sign}(D)$ |
| **$w_v$ (Excess Demand Volume)** | $[0, \mathbf{1}, 2, 3]$ | $1$ | Applies only when $|D| > 1$. For $D=+2$, $\lfloor (2-1)/2 \rfloor = 0$, so $\Delta P$ is invariant. For $D=+6$: $+2$ per unit step. | Applies only when $|D| > 1$. For $D=-6$: $-2$ per unit step. | High during panic or squeeze runs | Non-linear volume dampener |
| **$w_{\text{vol}}$ (Volatility Weight)** | $[0, \mathbf{1}, 2, 3]$ | $1$ | $+1$ per unit step when $\mathcal{V} \ge 4$ ($w_{\text{vol}}=0 \to \Delta P=+5$; $w_{\text{vol}}=3 \to \Delta P=+8$) | $-1$ per unit step when $\mathcal{V} \ge 4$ ($w_{\text{vol}}=0 \to \Delta P=-6$; $w_{\text{vol}}=3 \to \Delta P=-9$) | Low under standard volume | Multiplies trend momentum; strictly 0 when neutral |
| **$w_h$ (Health Weight)** | $[0, \mathbf{1}, 2, 3]$ | $1$ | Linear in $(H' - 5)$. For $H'=7$: $+2$ per unit step ($w_h=0 \to \Delta P=+4$; $w_h=3 \to \Delta P=+10$) | Linear in $(H' - 5)$. For $H'=3$: $-2$ per unit step ($w_h=0 \to \Delta P=-5$; $w_h=3 \to \Delta P=-11$) | High when company is distressed or prime | Powerful fundamental valuation anchor |
| **$w_s$ (Sentiment Weight)** | $[0, \mathbf{1}, 2, 3]$ | $1$ | Linear in $\lfloor \mathcal{S}'/2 \rfloor$. For $\mathcal{S}'=+1$: 0. For $\mathcal{S}'=+3$: $+1$ per unit step. | For $\mathcal{S}'=-1$: $-1$ per unit step. For $\mathcal{S}'=-3$: $-2$ per unit step. | Moderate in multi-round runs | Dampened multi-round feedback |
| **$w_l$ (Scarcity Surcharge)** | $[0, 1, \mathbf{2}, 3, 4]$ | $2$ | Zero effect when $\mathcal{L} \ge 20\%$. In low reserve ($\mathcal{L} < 20\%, D > 0$): $+1$ per unit step ($w_l=0 \to \Delta P=+7$; $w_l=4 \to \Delta P=+11$). | Exactly zero effect when $D \le 0$ even if $\mathcal{L} < 20\%$. | Low to moderate | Targeted anti-run scarcity premium |

### 5.2 Controlled Interaction Analysis

Four critical two-factor interactions were evaluated to identify compounding effects:

1. **Event Weight ($w_e$) with Volatility Weight ($w_{\text{vol}}$)**:
   - High event impact increases volatility directly via $\mathcal{V} = G + 2|e|$.
   - For $e=+2$ and $G=2$, $\mathcal{V} = 6 \implies \lfloor 6/4 \rfloor = 1$.
   - Increasing both $w_e$ and $w_{\text{vol}}$ creates super-linear momentum: at $(w_e=1, w_{\text{vol}}=0) \implies \Delta P = +5$; at $(w_e=3, w_{\text{vol}}=2) \implies \Delta P = +11$.
   - **Interaction Finding**: Volatility magnification acts as an amplifier of public events. When $e$ is large, $w_{\text{vol}}$ effectively increases the event slope by $50\%$.

2. **Demand-Volume Weight ($w_v$) with Volatility Weight ($w_{\text{vol}}$)**:
   - High order volume ($D=+6, G=6$) triggers both excess demand volume ($\lfloor 5/2 \rfloor = 2$) and volatility ($\lfloor 6/4 \rfloor = 1$).
   - At $(w_v=0, w_{\text{vol}}=0)$, volume-induced delta is $0$.
   - At candidate values $(w_v=1, w_{\text{vol}}=1)$, volume-induced delta is $+2 + 1 = +3$.
   - At high settings $(w_v=3, w_{\text{vol}}=3)$, volume-induced delta reaches $+6 + 3 = +9$.
   - **Interaction Finding**: Large one-sided trades experience compounded execution impact. This heavily penalizes massive single-round trade dumps or runs.

3. **Health Weight ($w_h$) with Sentiment Weight ($w_s$)**:
   - Company health and market sentiment frequently move together under prolonged market pressure.
   - For a distressed firm with negative sentiment ($H'=3, \mathcal{S}'=-3$):
     - At $(w_h=0, w_s=0)$: Fundamental terms add $0$.
     - At candidate $(w_h=1, w_s=1)$: Fundamental terms add $-2 + (-2) = -4$.
     - At high settings $(w_h=2, w_s=2)$: Fundamental terms add $-4 + (-4) = -8$.
   - **Interaction Finding**: Health and sentiment act as reinforcing macro anchors. In sustained declines, their combination accelerates price drops toward the lower bound.

4. **Scarcity Surcharge ($w_l$) with Starting Reserve ($\text{Reserve}$)**:
   - Evaluated across starting reserves $[50, 30, 21, 15, 10]$ with $D=+2$.
   - When closing reserve $\ge 20\%$, scarcity surcharge is strictly $0$ regardless of $w_l$.
   - When closing reserve drops below $20\%$ (e.g. $21 \to 19$), the surcharge activates as a discrete step function of size $+w_l$.
   - **Interaction Finding**: The scarcity surcharge is non-linear and threshold-based. It does not exert any gradual drag before the $20\%$ threshold, creating an abrupt 2-point price step when the pool crosses below $20\%$ remaining inventory.

---

## 6. Multi-Round Behavior and Sequence Traces

Six deterministic 3-round sequences (representing the canonical 3-round DARKPOOL match) were executed on both the approved baseline and the candidate proposal. Starting state for all sequences: $P_0 = 10, H_0 = 5, \text{Reserve} = 50, \text{Total Supply} = 100$.

### Sequence A: Neutral Sequence (e=0, Zero Trading)
*Tests for unexplained baseline drift, sentiment leakage, or volatility accumulation.*

| Round | Event $e$ | Executed Buys/Sells | $D / G$ | Opening $P$ | Baseline Close $P'$ | Candidate Close $P'$ (Raw $\Delta P$) | Opening $H$ | Closing $H'$ | Closing Res (Liq %) | Sentiment $\mathcal{S}'$ | Volatility $\mathcal{V}$ | Supply Conserved |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **R1** | $0$ | $0 / 0$ | $0 / 0$ | $10$ | $10$ | **10** ($0$) | $5$ | $5$ | $50$ ($50\%$) | $0$ | $0$ | **YES** |
| **R2** | $0$ | $0 / 0$ | $0 / 0$ | $10$ | $10$ | **10** ($0$) | $5$ | $5$ | $50$ ($50\%$) | $0$ | $0$ | **YES** |
| **R3** | $0$ | $0 / 0$ | $0 / 0$ | $10$ | $10$ | **10** ($0$) | $5$ | $5$ | $50$ ($50\%$) | $0$ | $0$ | **YES** |

- **Finding**: Zero drift observed. Price, health, sentiment, and volatility remain completely invariant across all 3 rounds.

### Sequence B: Persistent Positive Pressure (e=+1..+2, Net Buys)
*Tests multi-round bullish accumulation, health ceiling saturation, and price trajectory.*

| Round | Event $e$ | Executed Buys/Sells | $D / G$ | Opening $P$ | Baseline Close $P'$ | Candidate Close $P'$ (Raw $\Delta P$) | Opening $H$ | Closing $H'$ | Closing Res (Liq %) | Sentiment $\mathcal{S}'$ | Volatility $\mathcal{V}$ | Supply Conserved |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **R1** | $+1$ | $3 / 0$ | $+3 / 3$ | $10$ | $13$ | **17** ($+7$) | $5$ | $7$ | $47$ ($47\%$) | $+1$ | $5$ | **YES** |
| **R2** | $+2$ | $3 / 0$ | $+3 / 3$ | $17$ | $18$ | **30** ($+13$) | $7$ | $10$ | $44$ ($44\%$) | $+2$ | $7$ | **YES** |
| **R3** | $+1$ | $3 / 0$ | $+3 / 3$ | $30$ | $21$ | **41** ($+11$) | $10$ | $10$ | $41$ ($41\%$) | $+3$ | $5$ | **YES** |

- **Finding**: In R2, health reaches ceiling $H'=10$ and clamps cleanly. Candidate price rises from $10 \to 17 \to 30 \to 41$, significantly outpacing baseline ($10 \to 13 \to 18 \to 21$) due to fundamental health ($+5$) and sentiment ($+1$) terms. Share supply is conserved throughout ($41 \text{ reserve} + 59 \text{ player held} = 100$).

### Sequence C: Persistent Negative Pressure (e=-1, Net Sells)
*Tests multi-round bearish panic, health depletion, and price floor saturation.*

| Round | Event $e$ | Executed Buys/Sells | $D / G$ | Opening $P$ | Baseline Close $P'$ | Candidate Close $P'$ (Raw $\Delta P$) | Opening $H$ | Closing $H'$ | Closing Res (Liq %) | Sentiment $\mathcal{S}'$ | Volatility $\mathcal{V}$ | Supply Conserved |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **R1** | $-1$ | $0 / 2$ | $-2 / 2$ | $10$ | $7$ | **3** ($-7$) | $5$ | $3$ | $52$ ($52\%$) | $-1$ | $4$ | **YES** |
| **R2** | $-1$ | $0 / 2$ | $-2 / 2$ | $3$ | $4$ | **1** ($-9$) | $3$ | $1$ | $54$ ($54\%$) | $-2$ | $4$ | **YES** |
| **R3** | $-1$ | $0 / 2$ | $-2 / 2$ | $1$ | $1$ | **1** ($-11$) | $1$ | $0$ | $56$ ($56\%$) | $-3$ | $4$ | **YES** |

- **Finding**: Price rapidly drops to lower bound $P'=1$ in Round 2 and remains firmly clamped in Round 3 despite large raw negative delta ($-11$). At the end of Round 3, health reaches $0$, triggering canonical match termination (InsiderWin). Share supply is conserved ($56 \text{ reserve} + 44 \text{ player held} = 100$).

### Sequence D: Alternating Events and Demand (Reversibility Check)
*Tests for hysteresis, rounding bias, or drift under oscillating market pressure.*

| Round | Event $e$ | Executed Buys/Sells | $D / G$ | Opening $P$ | Baseline Close $P'$ | Candidate Close $P'$ (Raw $\Delta P$) | Opening $H$ | Closing $H'$ | Closing Res (Liq %) | Sentiment $\mathcal{S}'$ | Volatility $\mathcal{V}$ | Supply Conserved |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **R1** | $+2$ | $2 / 0$ | $+2 / 2$ | $10$ | $15$ | **19** ($+9$) | $5$ | $8$ | $48$ ($48\%$) | $+1$ | $6$ | **YES** |
| **R2** | $-2$ | $0 / 2$ | $-2 / 2$ | $19$ | $10$ | **13** ($-6$) | $8$ | $5$ | $50$ ($50\%$) | $0$ | $6$ | **YES** |
| **R3** | $+2$ | $2 / 0$ | $+2 / 2$ | $13$ | $15$ | **22** ($+9$) | $5$ | $8$ | $48$ ($48\%$) | $+1$ | $6$ | **YES** |

- **Finding**:
  - In R1, bull pressure pushes candidate price from $10 \to 19$ ($\Delta P = +9$).
  - In R2, symmetric bear pressure drops candidate price from $19 \to 13$ ($\Delta P = -6$).
  - **Asymmetry Explanation**: The $+9$ gain in R1 is driven by health reaching $8$ ($+3$ health term). In R2, health starts at $8$ and falls to $5$ ($0$ health term). Because health returns to baseline $5$, R2 does not experience a negative health discount. This reflects economic reality: health rebounded to neutral rather than becoming distressed. Baseline closes at $15 \to 10 \to 15$, showing exact baseline reversibility.

### Sequence E: Liquidity Depletion (Reserve Threshold Crossing)
*Demonstrates exact round-by-round crossing of the 20% reserve threshold and scarcity surcharge activation.*

| Round | Event $e$ | Executed Buys/Sells | $D / G$ | Opening $P$ | Baseline Close $P'$ | Candidate Close $P'$ (Raw $\Delta P$) | Opening $H$ | Closing $H'$ | Closing Res (Liq %) | Sentiment $\mathcal{S}'$ | Volatility $\mathcal{V}$ | Supply Conserved |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **R1** | $0$ | $15 / 0$ | $+15 / 15$ | $10$ | $11$ | **23** ($+13$) | $5$ | $6$ | $35$ ($35\%$) | $+3$ | $15$ | **YES** |
| **R2** | $0$ | $16 / 0$ | $+16 / 16$ | $23$ | $12$ | **42** ($+19$) | $6$ | $7$ | **19** ($19\%$) | $+6$ | $16$ | **YES** |
| **R3** | $0$ | $5 / 0$ | $+5 / 5$ | $42$ | $13$ | **55** ($+13$) | $7$ | $8$ | **14** ($14\%$) | $+8$ | $5$ | **YES** |

- **Threshold Crossing Demonstration**:
  - In R1, 15 shares bought $\to$ closing reserve drops to $35$ ($35\% \ge 20\%$). Surcharge is inactive ($\text{term}_l = 0$).
  - In R2, 16 shares bought $\to$ closing reserve drops to $19$ ($19\% < 20\%$). **Scarcity surcharge activates** ($\text{term}_l = +2$). Price reaches $42$.
  - In R3, 5 shares bought $\to$ closing reserve drops to $14$ ($14\% < 20\%$). Surcharge remains active ($\text{term}_l = +2$). Price reaches $55$.
  - Baseline price increases by only $+1$ per round ($10 \to 11 \to 12 \to 13$), wholly insensitive to reserve depletion.

### Sequence F: High-Volume, Low-Net-Demand Sequence (Gross Turnover Test)
*Tests gross volume indicator $\mathcal{V} = G + 2|e|$ when net demand remains neutral or low.*

| Round | Event $e$ | Executed Buys/Sells | $D / G$ | Opening $P$ | Baseline Close $P'$ | Candidate Close $P'$ (Raw $\Delta P$) | Opening $H$ | Closing $H'$ | Closing Res (Liq %) | Sentiment $\mathcal{S}'$ | Volatility $\mathcal{V}$ | Supply Conserved |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **R1** | $0$ | $2 / 2$ | $0 / 4$ | $10$ | $10$ | **10** ($0$) | $5$ | $5$ | $50$ ($50\%$) | $0$ | $4$ | **YES** |
| **R2** | $0$ | $5 / 2$ | $+3 / 7$ | $10$ | $11$ | **14** ($+4$) | $5$ | $6$ | $47$ ($47\%$) | $+1$ | $7$ | **YES** |
| **R3** | $0$ | $2 / 2$ | $0 / 4$ | $14$ | $11$ | **15** ($+1$) | $6$ | $6$ | $47$ ($47\%$) | $+1$ | $4$ | **YES** |

- **Finding**:
  - In R1 ($B=S=2$), gross volume is $4 \implies \mathcal{V} = 4$. However, because $D=0$ and $e=0$, $\text{dir}_{\text{market}} = 0$, guaranteeing zero price change ($\Delta P = 0$).
  - In R2, net demand is $+3$ and gross volume is $7 \implies \mathcal{V} = 7 \implies \lfloor 7/4 \rfloor = 1$. The volatility term adds $+1$, lifting closing price to $14$.
  - In R3, balanced trading resumes ($B=S=2$), yielding $\text{dir}_{\text{market}} = 0$. However, carryover health $H=6$ adds $+1$, moving price from $14 \to 15$.

---

## 7. Formal Invariant Verification Results

The automated harness (`darkpool_market_sensitivity_analysis --test`) executes comprehensive property checks across the complete combinatorial parameter space:

1. **Price Boundedness Invariant**:
   - Evaluated across $1,500+$ synthetic state permutations:
   - $P \in \{1, 10, 50, 90, 100\}$, $H \in \{0, 1, 5, 9, 10\}$, $e \in \{-2, -1, 0, 1, 2\}$, $D \in \{-10, \dots, 10\}$, $\mathcal{S} \in \{-10, \dots, 10\}$, $\mathcal{L} \in \{0, \dots, 100\}$, $\mathcal{V} \in \{0, \dots, 20\}$.
   - **Result**: $P' \in [1, 100]$ holds in 100% of cases. Zero out-of-bounds occurrences.
2. **Health Boundedness Invariant**:
   - $H' = \text{clamp}(H + e + \text{sign}(D), 0, 10) \in [0, 10]$ verified in all single-round and multi-round evaluations.
3. **Finite Share Conservation**:
   - Verified that $\sum \text{player\_shares} + \text{reserve} = \text{total\_supply}$ holds identically before and after every market clearing step.
   - Note: Credit counters are not conserved, adhering to canonical game economy rule DEC-19.
4. **Replay Determinism**:
   - Consecutive runs with identical starting fixtures and action logs produce byte-identical prices, market indicators, and receipts.
5. **Zero Neutral Drift**:
   - When $e=0$ and $D=0$, $\Delta P \equiv 0$ regardless of gross trading volume $G$ or measured volatility $\mathcal{V}$.
6. **Mathematical Floor Division Semantics**:
   - Verified that integer arithmetic satisfies mathematical floor semantics for negative numbers: $\lfloor -1/2 \rfloor = -1, \lfloor -3/2 \rfloor = -2, \lfloor -5/2 \rfloor = -3$.
7. **Baseline Isolation**:
   - Default match creation and coordinator dispatch instantiate `MarketModelCoefficients::approved_baseline()`.
   - Candidate model configuration in a match instance does not alter global state or affect subsequent match creations.

---

## 8. Discoveries, Model Limitations, and Design Recommendations

### 8.1 Measured Implementation Findings

1. **Mirrored Pair 1-Point Delta Asymmetry**:
   - Due to mathematical floor division, symmetric positive ($e=+1, D=+2$) vs negative ($e=-1, D=-2$) shocks produce $\Delta P = +6$ vs $\Delta P = -7$.
   - This occurs because $\lfloor +1/2 \rfloor = 0$ while $\lfloor -1/2 \rfloor = -1$.
   - *Recommendation*: If exact numerical symmetry is desired by the game designer, an odd-symmetric division rule $\text{sign}(x) \cdot \lfloor |x| / 2 \rfloor$ could be considered; however, the current mathematical floor division is rigorously deterministic, safe, and adheres to standard mathematical specifications.

2. **Discrete Scarcity Surcharge Step**:
   - The low-reserve scarcity surcharge $+w_l = +2$ activates abruptly at $\mathcal{L} < 20\%$.
   - There is no graduated escalation (e.g. at $30\%$ or $10\%$).
   - *Recommendation*: A single 2-point step is simple, memorable for players, and avoids excess arithmetic in mental calculation. Retaining the 2-point discrete surcharge is recommended if candidate weights are adopted.

3. **Lower Bound Squeeze on Distressed Firms**:
   - In Sequence C, negative events combined with net selling drove price from $10 \to 3 \to 1$ in just 2 rounds.
   - *Recommendation*: The candidate model responds much more vigorously to fundamental distress than the baseline model ($10 \to 7 \to 4$). This increases gameplay tension for the Insider and Employees, but designers should note that low-price stocks can pin to $P=1$ rapidly.

### 8.2 Summary

This analysis provides quantitative evidence for later design decisions. **No candidate coefficients have been activated in ordinary gameplay, and the approved baseline remains active by default.**
