# DARKPOOL Market Price Model Proposal (Stage 4.2 Reconciled Proposal)

> [!WARNING]
> **PROPOSED COEFFICIENTS AND CANDIDATE RULES PENDING USER REVIEW:**
> The pricing factors (Sentiment, Volatility, Liquidity, Company Health, Business Event Impact) and deterministic calculation architecture are user-approved. However, the **exact numerical coefficients**, the **volatility pricing term**, and the **positive low-reserve scarcity surcharge** documented in this proposal are **candidate design proposals awaiting explicit user review and approval**. They are not yet approved canonical game rules.
> The engine baseline defaults strictly to `MarketModelCoefficients::approved_baseline()`, preserving exact backward compatibility with all canonical rules and prior test suites until candidate values are formally reviewed and accepted.
> For exhaustive empirical data, parameter sensitivity, and multi-round sequence traces, see [Reproducible Market-Balance and Sensitivity Analysis (Stage 4.3)](market-model-sensitivity.md).

---

## 1. Architectural Overview and Design Principles

The Stage 4 market model provides a deterministic, multi-factor market-clearing and pricing engine. The model computes:
1. **Closing Health ($H'$)**: Authoritative update from opening health, public business event impact, and direction of net demand.
2. **Market Indicators**:
   - **Liquidity ($\mathcal{L}$)**: Percentage of finite total share supply held in the market reserve.
   - **Sentiment ($\mathcal{S}$)**: Cumulative market mood updated deterministically from trade demand pressure.
   - **Volatility ($\mathcal{V}$)**: Non-negative measure of recent price and event pressure grounded in gross executed trading volume and event magnitude.
3. **Closing Stock Price ($P'$)**: Multi-factor deterministic price adjustment incorporating fundamental company health, event impact, net demand volume, volatility magnification, sentiment, and reserve depth.

### Determinism Invariants
- **Strict Integer Arithmetic with Mathematical Floor Division**: All division operations follow mathematical floor ($\lfloor a / b \rfloor$), implemented via the safe helper `Match::floor_div(a, b)`. C++ truncating integer division `/` truncates negative values toward zero (e.g. `-1 / 2 == 0`, `-3 / 2 == -1`), which violates the mathematical model. True mathematical floor division correctly yields $\lfloor -1/2 \rfloor = -1$, $\lfloor -3/2 \rfloor = -2$, $\lfloor -7/2 \rfloor = -4$, $\lfloor -9/2 \rfloor = -5$.
- **Overflow & Precondition Safety**: `Match::floor_div` computes in 64-bit integer arithmetic and clamps results to `[INT32_MIN, INT32_MAX]`, preventing signed integer overflow on `INT32_MIN / -1`. Division by zero throws `std::invalid_argument` rather than silently returning 0.
- **Zero Hidden Randomness**: No timestamps, pointers, floating-point nondeterminism, network arrival orders, or unordered container iterations affect pricing.
- **Strict Bounds**:
  - Company Health: $H \in [0, 10]$
  - Stock Price: $P \in [1, 100]$
  - Sentiment: $\mathcal{S} \in [-10, 10]$
  - Volatility: $\mathcal{V} \in [0, 20]$
  - Liquidity: $\mathcal{L} \in [0, 100]$ (percentage)

---

## 2. Mathematical Specification

### 2.1 Trade Quantities, Volume, and Health Transition

Let $B$ be total executed buy volume (shares) and $S$ be total executed sell volume (shares).
- **Net Trade Demand**:
  $$D = B - S$$
  $$\text{sign}(D) = \begin{cases} +1 & \text{if } D > 0 \\ 0 & \text{if } D = 0 \\ -1 & \text{if } D < 0 \end{cases}$$
- **Gross Executed Volume**:
  $$G = B + S$$

Company health transitions authoritatively according to the approved canonical rule:
$$H' = \text{clamp}(H + e + \text{sign}(D), 0, 10)$$
where $e \in \{-2, -1, 0, 1, 2\}$ is the authoritative business event impact.

### 2.2 Market Indicators

1. **Liquidity ($\mathcal{L}$)**: Grounded strictly in actual remaining reserve shares relative to total finite supply:
   $$\mathcal{L} = \text{clamp}\left(\left\lfloor \frac{\text{Reserve} \times 100}{\text{Total Supply}} \right\rfloor, 0, 100\right)$$
   For a standard 5-player match with 100 total shares, each reserve share equals exactly 1% liquidity.

2. **Sentiment ($\mathcal{S}$)**: Updated each market resolution from net demand direction and volume using mathematical floor division:
   $$\Delta \mathcal{S} = \begin{cases} \text{sign}(D) & \text{if } |D| = 1 \\ \text{clamp}\left(\lfloor \frac{D}{2} \rfloor, -3, 3\right) & \text{if } |D| > 1 \\ 0 & \text{if } D = 0 \end{cases}$$
   $$\mathcal{S}' = \text{clamp}(\mathcal{S} + \Delta \mathcal{S}, -10, 10)$$

3. **Volatility Indicator ($\mathcal{V}$)**: Measures aggregate market activity and event uncertainty using gross executed volume $G = B + S$:
   $$\mathcal{V} = \text{clamp}(G + 2 \cdot |e|, 0, 20)$$
   Grounding volatility in gross volume $G$ guarantees that all executed trades contribute to measured volatility, ensuring that balanced buy and sell trading ($B = S > 0$) is transparently reflected.

---

### 2.3 Candidate Closing Price Formula

$$\Delta P = w_e \cdot e + w_d \cdot \text{sign}(D) + w_v \cdot \left(\text{sign}(D) \cdot \left\lfloor \frac{|D| - 1}{2} \right\rfloor \right) + w_{\text{vol}} \cdot \left(\text{dir}_{\text{market}} \cdot \left\lfloor \frac{\mathcal{V}}{4} \right\rfloor \right) + w_h \cdot (H' - 5) + w_s \cdot \left\lfloor \frac{\mathcal{S}'}{2} \right\rfloor + w_l \cdot \text{ScarcitySurcharge}_{\mathcal{L}}$$

where market direction is established by order flow, falling back to public business event impact when trading is neutral:
$$\text{dir}_{\text{market}} = \begin{cases} \text{sign}(D) & \text{if } D \ne 0 \\ \text{sign}(e) & \text{if } D = 0 \text{ and } e \ne 0 \\ 0 & \text{if } D = 0 \text{ and } e = 0 \end{cases}$$

and the candidate scarcity surcharge applies when buyers demand shares from a severely depleted reserve:
$$\text{ScarcitySurcharge}_{\mathcal{L}} = \begin{cases} 1 & \text{if } D > 0 \text{ and } \mathcal{L} < 20\% \\ 0 & \text{otherwise} \end{cases}$$

Finally, price is clamped to the canonical bounds:
$$P' = \text{clamp}(P + \Delta P, 1, 100)$$

---

### 2.4 Candidate Design Decisions & Reconciled Mechanisms

#### 1. Candidate Decision AD-S4-09: Positive Scarcity Surcharge (`+w_l`) vs Negative Illiquidity Discount (`-w_l`)
- **Discrepancy Identification**: The Stage 4.1 report text previously wrote the liquidity term as a negative penalty (`- liquidity_penalty`), yet Scenario 7B applied $+2$ yielding $P'=23$ (or $P'=24$ with volatility), creating a sign conflict between text and code.
- **Design Rationale**: In market microstructure, buying into a depleted dark pool inventory ($\mathcal{L} < 20\%$) causes acute physical share shortage, supply squeeze, and upward execution slippage (+2 credits/share). A negative adjustment would award buyers an artificial discount when shares are scarce, contradicting fundamental economics.
- **Gameplay Consequences**: Applying a positive scarcity surcharge makes buying the last remaining shares in the pool increasingly expensive, incentivizing players to time their purchases before pool exhaustion and penalizing reckless demand runs.
- **Status**: Formally identified as candidate decision **AD-S4-09** and retained as an unapproved candidate proposal pending explicit user review. Baseline leaves $w_l = 0$.

#### 2. Candidate Decision AD-S4-10: Deterministic Volatility Magnification Factor ($w_{\text{vol}}$)
- **Design Rationale**: The approved design requires volatility as a pricing factor. Adding unsigned volatility $\mathcal{V}$ directly would systematically inflate prices, destroying market neutrality. Multiplying by market direction $\text{dir}_{\text{market}}$ ensures:
  1. **Zero Price Bias in Neutral Markets**: When $D = 0$ and $e = 0$, $\text{dir}_{\text{market}} = 0$, guaranteeing that volatility adds exactly 0 price impact even under heavy balanced trading turnover ($B = S > 0 \implies G > 0$).
  2. **Directional Magnification**: When order flow or event impact establishes a directional trend, higher volatility proportionally magnifies the price swing via $\lfloor \mathcal{V} / 4 \rfloor \in [0, 5]$.
  3. **Distinct from Volume Weight $w_v$**: $w_v$ reflects excess net demand quantity ($|D| > 1$), whereas $w_{\text{vol}}$ reflects gross market activity and uncertainty ($G + 2|e|$).
- **Concrete Demonstration**: In Scenario 5 ($D = +3$), low turnover ($G = 3 \implies \mathcal{V} = 3 \implies \lfloor 3/4 \rfloor = 0$) results in $P' = 23$, whereas high turnover ($G = 7 \implies \mathcal{V} = 7 \implies \lfloor 7/4 \rfloor = 1$) results in $P' = 24$, demonstrating that volatility changes closing price when gross volume changes while net demand remains equal.
- **Status**: Formally identified as candidate decision **AD-S4-10** and retained as an unapproved candidate proposal pending explicit user review. Baseline leaves $w_{\text{vol}} = 0$.

---

## 3. Centralized Coefficients Table

| Coefficient | Parameter Name in C++ | Baseline (Approved) | Candidate Proposal | Description |
| :--- | :--- | :---: | :---: | :--- |
| $w_e$ | `event_weight` | `2` | `2` | Business event multiplier |
| $w_d$ | `demand_weight` | `1` | `1` | Net demand directional impact $\text{sign}(B - S)$ |
| $w_v$ | `volume_weight` | `0` | `1` | Excess net demand volume impact $\text{sign}(D) \cdot \lfloor (|D|-1)/2 \rfloor$ |
| $w_{\text{vol}}$ | `volatility_weight` | `0` | `1` | Volatility magnification $\text{dir}_{\text{market}} \cdot \lfloor \mathcal{V}/4 \rfloor$ |
| $w_h$ | `health_weight` | `0` | `1` | Health deviation from baseline ($H' - 5$) |
| $w_s$ | `sentiment_weight` | `0` | `1` | Market sentiment influence $\lfloor \mathcal{S}/2 \rfloor$ |
| $w_l$ | `liquidity_penalty` | `0` | `2` | Low-reserve scarcity surcharge when buying and reserve $< 20\%$ |

### Code Centralization
All coefficients are encapsulated in `struct MarketModelCoefficients` (`include/darkpool/game_engine.hpp`):
- `MarketModelCoefficients::approved_baseline()` returns `{2, 1, 0, 0, 0, 0, 0}`.
- `MarketModelCoefficients::candidate_proposal()` returns `{2, 1, 1, 1, 1, 1, 2}`.
All constructors in `Match`, `from_state`, and `MatchCoordinator` default strictly to `approved_baseline()`.
`Match::calculate_price()` defaults strictly to `approved_baseline()`.

---

## 4. Reproducible Scenario Analysis

### 4.1 Reconciled Candidate Model Simulations (13 Reference Scenarios)

The table below reports deterministic simulation results comparing opening state $(P, H)$ to closing state $(P', H')$ under the proposed candidate model across the evaluation scenarios:

| # | Scenario Description | Opening $P, H$ | Event $e$ | Executed Buys / Sells ($D, G$) | Sentiment $\mathcal{S}'$ | Reserve / Liquidity $\mathcal{L}$ | Volatility $\mathcal{V}$ | Closing Health $H'$ | $\Delta P$ Components ($w_e \cdot e, w_d \cdot D_{\text{dir}}, w_v, w_{\text{vol}}, w_h, w_s, w_l$) | Closing Price $P'$ | Direction / Magnitude |
| :-: | :--- | :--- | :---: | :--- | :---: | :---: | :---: | :---: | :---: | :--- | :---: | :--- |
| **1** | **Strong Positive Event** | 10, 5 | +2 | 1 buy, 0 sell ($+1, 1$) | +1 | 49 / 49% | 5 | 8 | $+4, +1, 0, +1, +3, 0, 0$ | **19** | Bullish (+9) |
| **2** | **Strong Negative Event** | 10, 5 | -2 | 0 buy, 1 sell ($-1, 1$) | -1 | 51 / 51% | 5 | 2 | $-4, -1, 0, -1, -3, -1, 0$ | **1** | Bearish (-10, clamped) |
| **3** | **Neutral Market** | 10, 5 | 0 | 0 buy, 0 sell ($0, 0$) | 0 | 50 / 50% | 0 | 5 | $0, 0, 0, 0, 0, 0, 0$ | **10** | Neutral (0 change) |
| **4** | **High Sentiment Bull Run** | 20, 7 | +1 | 4 buy, 0 sell ($+4, 4$) | +7 | 46 / 46% | 6 | 9 | $+2, +1, +1, +1, +4, +3, 0$ | **32** | Bullish (+12) |
| **5** | **Low Sentiment Bear Panic** | 20, 3 | -1 | 0 buy, 4 sell ($-4, 4$) | -7 | 54 / 54% | 6 | 1 | $-2, -1, -1, -1, -4, -4, 0$ | **7** | Bearish (-13) |
| **6** | **Volatile Market** | 50, 5 | +2 | 6 buy, 0 sell ($+6, 6$) | +3 | 44 / 44% | 10 | 8 | $+4, +1, +2, +2, +3, +1, 0$ | **63** | High Momentum (+13) |
| **7** | **Stable Market** | 50, 5 | 0 | 1 buy, 1 sell ($0, 2$) | 0 | 50 / 50% | 2 | 5 | $0, 0, 0, 0, 0, 0, 0$ | **50** | Stable (0 change) |
| **8** | **High Liquidity Reserve** | 15, 6 | +1 | 2 buy, 0 sell ($+2, 2$) | +1 | 68 / 68% | 4 | 8 | $+2, +1, 0, +1, +3, 0, 0$ | **22** | Smooth fill (+7) |
| **9** | **Low Liquidity Squeeze** | 15, 6 | +1 | 2 buy, 0 sell ($+2, 2$) | +1 | 10 / 10% | 4 | 8 | $+2, +1, 0, +1, +3, 0, +2$ (surcharge) | **24** | Reserve shortage (+9) |
| **10** | **Healthy Company ($H=9$)**| 30, 9 | 0 | 1 buy, 0 sell ($+1, 1$) | +1 | 49 / 49% | 1 | 10 | $0, +1, 0, 0, +5, 0, 0$ | **36** | Premium valuation (+6) |
| **11** | **Distressed Company ($H=2$)**| 30, 2 | 0 | 0 buy, 1 sell ($-1, 1$) | -1 | 51 / 51% | 1 | 1 | $0, -1, 0, 0, -4, -1, 0$ | **24** | Distress discount (-6) |
| **12** | **Upper Bound Clamping** | 95, 8 | +2 | 5 buy, 0 sell ($+5, 5$) | +2 | 45 / 45% | 9 | 10 (clamp) | $+4, +1, +2, +2, +5, +1, 0 \implies \Delta P=+15$ | **100** (clamp) | Saturated at cap 100 |
| **13** | **Lower Bound Clamping** | 5, 2 | -2 | 0 buy, 5 sell ($-5, 5$) | -3 | 55 / 55% | 9 | 0 (clamp) | $-4, -1, -2, -2, -5, -2, 0 \implies \Delta P=-16$ | **1** (clamp) | Saturated at floor 1 |

---

### 4.2 Targeted Nine Before/After Audit Scenarios

Below are the 9 specific scenarios reconciling Stage 4.1 vs Stage 4.2 Reconciled candidate behaviors:

| Scenario | Complete Initial State | Actions / Event | Stage 4.1 Candidate | Stage 4.2 Reconciled Candidate | Reconciliation & Governance Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1. No trading and neutral event** | $P=10, H=5, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$ | $e=0, B=0, S=0$ ($D=0, G=0$) | $\mathcal{V}=0, P'=10, H'=5$ | $\mathcal{V}=0, P'=10, H'=5$ | Identical; baseline equilibrium preserved. |
| **2. Equal gross buys/sells ($D=0$)** | $P=50, H=5, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$ | $e=0, B=2, S=2$ ($D=0, G=4$) | $\mathcal{V}=4, P'=50, H'=5$ | $\mathcal{V}=\mathbf{4}, P'=50, H'=5$ | Gross volume $G=4$ yields $\mathcal{V}=4$. Because $D=0$ and $e=0$, $\text{dir}_{\text{market}}=0$, strictly guaranteeing zero price bias. |
| **3. Large positive net demand** | $P=50, H=5, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$ | $e=0, B=6, S=0$ ($D=+6, G=6$) | $\mathcal{V}=6, P'=55, H'=6$ | $\mathcal{V}=6, P'=\mathbf{56}, H'=6$ | $\mathcal{V}=6 \implies \lfloor 6/4 \rfloor = 1 \implies \text{vol\_term} = +1$. $\Delta P = +1 + 2 + 1 + 1 + 1 = +6 \implies P'=56$. |
| **4. Large negative net demand** | $P=50, H=5, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$ | $e=0, B=0, S=6$ ($D=-6, G=6$) | $\mathcal{V}=6, P'=44, H'=4$ | $\mathcal{V}=6, P'=\mathbf{43}, H'=4$ | $\mathcal{V}=6 \implies \lfloor 6/4 \rfloor = 1 \implies \text{vol\_term} = -1$. $\Delta P = -1 - 2 - 1 - 1 - 2 = -7 \implies P'=43$. |
| **5. Same net demand, different gross volume** | $P=20, H=5, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$ | **5A:** $e=0, B=3, S=0$ ($D=+3, G=3$)<br>**5B:** $e=0, B=5, S=2$ ($D=+3, G=7$) | **5A:** $\mathcal{V}=3, P'=23$<br>**5B:** $\mathcal{V}=7, P'=23$ (volatility was ignored) | **5A:** $\mathcal{V}=3, P'=\mathbf{23}$<br>**5B:** $\mathcal{V}=\mathbf{7}, P'=\mathbf{24}$ | **Reconciled:** 5A has $\lfloor 3/4 \rfloor = 0$ ($P'=23$). 5B has $\lfloor 7/4 \rfloor = 1$ ($P'=24$). Volatility explicitly changes closing price when gross turnover increases with constant net demand. |
| **6. Positive vs Negative events** | $P=10, H=5, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$ | **6A:** $e=+2, B=1, S=0$ ($D=+1, G=1$)<br>**6B:** $e=-2, B=0, S=1$ ($D=-1, G=1$) | **6A:** $P'=18, H'=8$<br>**6B:** $P'=1, H'=2$ | **6A:** $P'=\mathbf{19}, H'=8$<br>**6B:** $P'=\mathbf{1}, H'=2$ | In 6A: $\mathcal{V}=5 \implies \text{vol\_term} = +1 \implies P'=19$. In 6B: $\mathcal{V}=5 \implies \text{vol\_term} = -1 \implies P'=1$ (clamped). |
| **7. Low vs High reserve liquidity** | **7A:** $P=15, H=6, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=70, \text{Total}=100$<br>**7B:** $P=15, H=6, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=12, \text{Total}=100$ | $e=+1, B=2, S=0$ ($D=+2, G=2, \mathcal{V}=4$) | **7A:** $P'=21$<br>**7B:** $P'=23$ (unreconciled sign text) | **7A:** $P'=\mathbf{22}$<br>**7B:** $P'=\mathbf{24}$ | **Reconciled:** $\mathcal{V}=4 \implies \text{vol\_term}=+1$. 7B incurs candidate $+2$ scarcity surcharge ($\text{Res}'=10 < 20\%$), yielding $P'=24$ vs $P'=22$ for high reserve. Difference is strictly $+2$. |
| **8. Healthy vs Distressed state** | $P=30, H=9, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$<br>**8B:** $P=30, H=2, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$ | **8A:** $e=0, B=1, S=0$ ($D=+1$)<br>**8B:** $e=0, B=0, S=1$ ($D=-1$) | **8A:** $P'=36$<br>**8B:** $P'=24$ | **8A:** $P'=36$<br>**8B:** $P'=24$ | In both cases, $\mathcal{V}=1 \implies \lfloor 1/4 \rfloor = 0 \implies \text{vol\_term}=0$. Price impacts match Stage 4.1. |
| **9. Upper vs Lower price bounds** | **9A:** $P=95, H=8, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$<br>**9B:** $P=5, H=2, \mathcal{S}=0, \mathcal{V}=0,$ $\text{Res}=50, \text{Total}=100$ | **9A:** $e=+2, B=5, S=0$ ($D=+5$)<br>**9B:** $e=-2, B=0, S=5$ ($D=-5$) | **9A:** $P'=100$ (clamp)<br>**9B:** $P'=1$ (clamp) | **9A:** $P'=100$ (clamp)<br>**9B:** $P'=1$ (clamp) | Strict bounded clamping $P \in [1, 100]$ verified. |

---

## 5. Summary for Review

1. **Replay Determinism**: Verified by comprehensive deterministic unit tests in `tests/engine_tests.cpp`.
2. **Backward Compatibility**: Calling `Match` without parameters defaults to `MarketModelCoefficients::approved_baseline()`, guaranteeing 100% test compatibility with prior stages.
3. **Candidate Status**: Candidate coefficients ($w_e=2, w_d=1, w_v=1, w_{\text{vol}}=1, w_h=1, w_s=1, w_l=2$), the positive low-reserve scarcity surcharge rule, and the volatility pricing factor remain candidate proposals awaiting explicit user approval.
