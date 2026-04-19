# RFC 005: Vicsek Swarm Intelligence — Emergent Multi-Agent Coordination for N-Tier Complexity

## Status: Accepted
## Author: Khalil Warren (🦎 Sal)
## Date: 2026-04-15 (promoted 2026-04-19)
## Extends: RFC-004 (necto actor system)
## Depends: RFC-001 (celer-mem core), RFC-002 (streaming)

---

## 1. Abstract

This RFC extends **`celer::necto`** (RFC-004) with swarm coordination primitives — **`celer::necto::swarm`** — applying the **Vicsek self-propelled-particle model** and **ant-colony optimization** (ACO) principles to achieve emergent coordination across n-tier complexity tasks with **zero-shot efficacy** on novel, previously unseen tasks.

Where RFC-004 defined the actor substrate (ActorSystem, Mailbox, Envelope, tick/drain), this RFC defines what happens when a *population* of actors must coordinate on a complex task without a central planner. The swarm primitives are not a layer above necto — they are necto actors with specific interaction rules baked in. A `SwarmAgent` *is* a `necto::Actor`. The `Swarm` coordinator *is* an `ActorSystem` with additional tick phases. The pheromone graph *is* a celer-mem store accessed through actor contexts.

The central insight: classical agent orchestration (supervisor trees, DAG schedulers, centralized planners) collapses under combinatorial task spaces because the planner must *know* the task structure a priori. Biological swarms solve tasks they've never seen before — no ant has a blueprint for a bridge, yet they build one. They achieve this through **local interaction rules**, **stigmergic memory**, and **phase-transition alignment dynamics** described by Vicsek et al. (1995).

We formalize these biological mechanisms into three composable C++ primitives:

1. **`SwarmField`** — A 2D/n-dimensional alignment field where agent heading vectors converge via the Vicsek update rule. Each agent's "heading" is its current task-strategy vector. Alignment creates emergent consensus without centralized voting.

2. **`PheromoneGraph`** — A weighted directed graph backed by celer-mem storage where agents deposit and follow pheromone trails. Trails encode discovered task decompositions, successful tool-use sequences, and known-good solution paths. Trails evaporate, preventing lock-in.

3. **`MorphScheduler`** — A polymorphic division-of-labor system inspired by eusocial colonies (Hölldobler & Wilson, 1990). Agents self-assign to morphs (explorer, worker, evaluator, swarm_coordinator) based on local stimulus-response thresholds, dynamically rebalancing as task demands shift.

```cpp
#include <celer/necto/swarm/field.hpp>
#include <celer/necto/swarm/pheromone.hpp>
#include <celer/necto/swarm/morph.hpp>

// Build a swarm of 64 agents over a novel task space
celer::necto::swarm::Config cfg{
    .population      = 64,
    .alignment_radius = 0.3,    // Vicsek r — fraction of heading space
    .noise_eta        = 0.1,    // Vicsek η — exploration noise
    .evaporation_rate = 0.05,   // ACO ρ — pheromone decay per tick
    .morph_thresholds = {       // stimulus → morph transition thresholds
        {"explorer",  0.8},
        {"worker",    0.4},
        {"evaluator", 0.6},
        {"swarm_coordinator",     0.95},
    },
};

celer::necto::swarm::Swarm swarm(cfg, sys, store);

// Inject a novel task — the swarm has never seen this before
swarm.inject_task(Task{
    .description = "Decompose and implement a distributed rate limiter",
    .constraints = {"< 5ms p99 latency", "crash-consistent", "multi-region"},
    .tier        = TaskTier::N4,  // requires 4 levels of decomposition
});

// Run the swarm — agents self-organize, deposit trails, align strategies
auto result = swarm.run_to_convergence(max_rounds = 5000);
// result.solution : TaskTree — the emergent decomposition
// result.trails   : ranked solution paths from pheromone graph
// result.stats    : {rounds, phase_transitions, morph_distribution}
```

**What this is:** A coordination substrate that enables agent populations to solve complex, multi-level tasks they've never encountered before, through emergent self-organization rather than pre-programmed plans.

**What this is NOT:** An LLM prompt router, a DAG scheduler, or a supervisor tree. There is no central brain. The intelligence is in the interaction rules.

---

## 2. Problem Statement

### 2.1 Central Planning Fails at N-Tier Complexity

Current multi-agent frameworks organize agents via explicit hierarchies:

| Framework | Pattern | Failure Mode |
|-----------|---------|-------------|
| AutoGen | Conversation graph with supervisor | Supervisor must know the task DAG a priori |
| CrewAI | Role-based pipeline | Roles are hardcoded; novel tasks require new role definitions |
| LangGraph | Explicit state machine | State transitions must be enumerated at design time |
| MetaGPT | SOP-based workflow | Standard Operating Procedures assume known procedures |
| Swarm (OpenAI) | Handoff-based routing | Linear handoff chains, no parallel decomposition |

All of these assume the task decomposition is **known ahead of time**. For a 1-tier task ("summarize this document"), that works. For an n-tier task ("design and implement a distributed rate limiter that handles multi-region failover"), the decomposition tree has $O(b^n)$ nodes where $b$ is the branching factor and $n$ is the tier depth. No hardcoded DAG covers this.

**The 0-shot problem:** When an agent swarm encounters a *novel* task — one not in its training distribution — centralized planners fail because they cannot decompose what they don't recognize. Biological swarms succeed because decomposition is *emergent*, not planned.

### 2.2 Nature Solved This

Biological systems that coordinate without central planning:

| System | Scale | Mechanism | Relevant Property |
|--------|-------|-----------|-------------------|
| **Bird flocking** (Vicsek, 1995) | 10^3 - 10^5 | Velocity alignment within radius | Phase transition from disorder → order |
| **Ant foraging** (Deneubourg, 1990) | 10^4 - 10^6 | Pheromone trail deposition/following | Path optimization, no global planner |
| **Termite mound building** | 10^5 - 10^6 | Stigmergy (environment-mediated coordination) | Multi-tier construction from local rules |
| **Bee waggle dance** | 10^4 | Movement-encoded information sharing | Distributed consensus on resource location |
| **Army ant bridges** | 10^3 | Body-based adaptation | Zero-shot structural solutions |
| **Slime mold (Physarum)** | Continuous | Tube network optimization | Solves shortest-path problems it has never seen |

Common principles:
1. **No individual knows the plan.** Each agent follows simple local rules.
2. **Information is in the environment** (stigmergy), not in a central database.
3. **Phase transitions** create sudden coordination from noise.
4. **Division of labor** is dynamic — ants switch tasks based on local need, not assigned roles.
5. **Evaporation** prevents lock-in — old trails fade, allowing adaptation.

### 2.3 The Vicsek Model — Phase Transitions in Swarms

The Vicsek model (1995) is the minimal model for collective motion. $N$ self-propelled particles move at constant speed $v_0$, updating their heading angle at each timestep:

$$\theta_i(t+1) = \langle \theta_j(t) \rangle_{|r_{ij}| < R} + \eta \xi_i$$

Where:
- $\theta_i(t)$ is agent $i$'s heading at time $t$
- The average $\langle \cdot \rangle$ is taken over all agents $j$ within radius $R$ of agent $i$
- $\eta$ is the noise amplitude
- $\xi_i \sim \text{Uniform}(-\pi, \pi)$ is random noise

The critical result: there exists a **phase transition** at a critical noise level $\eta_c(N, R)$ below which the swarm spontaneously aligns into ordered collective motion. Above $\eta_c$, agents move randomly. Below it, a global consensus emerges from purely local interactions.

**Our application:** Replace "heading angle" with "task-strategy vector" in an n-dimensional embedding space. Agents exploring a novel task start with random strategies (disordered phase). As agents within local neighborhoods share results, successful strategies align (ordered phase). The phase transition IS the swarm discovering a viable decomposition.

### 2.4 Why This Cannot Be Built Without celer-mem

| Dependency | Why Required |
|------------|-------------|
| **celer-mem storage** | Pheromone graph needs persistent, structured KV storage. Trails must survive process restarts. |
| **celer::stream** (RFC-002) | Large pheromone scans and trail enumeration are streaming operations — materializing the entire graph kills memory |
| **celer::necto** (RFC-004) | Swarm agents ARE necto actors. The swarm extends the actor system, it doesn't wrap it. |
| **Okasaki immutability** | Pheromone snapshots for concurrent reads. Workers read the graph while explorers deposit new trails. |
| **constexpr vtable** | Agent morphs (Explorer, Worker, Evaluator, SwarmCoordinator) are concept-checked structs — same `ActorVTable` + `ActorHandle` pattern from RFC-004. No new type erasure machinery. |

---

## 3. Mathematical Foundation

### 3.1 The Generalized Vicsek Update Rule

We generalize from 2D heading angles to n-dimensional strategy vectors. Each agent $i$ maintains a unit vector $\hat{v}_i \in \mathbb{R}^d$ representing its current approach to the task.

At each tick:

$$\hat{v}_i(t+1) = \text{normalize}\left( \sum_{j \in \mathcal{N}_i(R)} w_{ij} \hat{v}_j(t) + \eta \boldsymbol{\xi}_i \right)$$

Where:
- $\mathcal{N}_i(R) = \{j : \|s_i - s_j\| < R\}$ is the neighborhood in **strategy space** (not physical space)
- $w_{ij} = \text{pheromone}(j) \cdot \text{quality}(j)$ weights successful agents higher
- $\boldsymbol{\xi}_i \sim \mathcal{N}(0, I_d)$ is isotropic Gaussian noise
- $\eta \in [0, 1]$ controls exploration vs. exploitation

**Key departure from vanilla Vicsek:** We weight the alignment by agent quality scores (estimated solution fitness). This biases the phase transition toward *correct* alignment rather than arbitrary consensus. Pure Vicsek can align on a wrong direction; quality-weighted Vicsek aligns on the best-performing direction.

### 3.2 Order Parameter — Detecting Convergence

The global order parameter $\phi$ measures alignment:

$$\phi(t) = \frac{1}{N} \left\| \sum_{i=1}^{N} \hat{v}_i(t) \right\|$$

- $\phi \approx 0$: disordered — agents exploring randomly (high noise / early phase)
- $\phi \approx 1$: ordered — agents have converged on a strategy consensus

The swarm has "solved" the task when $\phi$ exceeds a threshold $\phi^* \in [0.85, 0.95]$ for a sustained window. Premature convergence (high $\phi$ with low quality scores) triggers a **noise injection** event that temporarily increases $\eta$, breaking the lock-in and restarting exploration.

### 3.3 Pheromone Dynamics — Ant Colony Optimization

The pheromone graph $G = (V, E)$ where:
- Vertices $V$ are **task nodes** — sub-tasks, tool invocations, partial solutions, decomposition branches
- Edges $E$ connect parent tasks to sub-tasks, with pheromone weights $\tau_{ij}$

Pheromone update (Dorigo & Stützle, 2004):

$$\tau_{ij}(t+1) = (1 - \rho) \cdot \tau_{ij}(t) + \sum_{k=1}^{N} \Delta\tau_{ij}^k$$

Where:
- $\rho \in (0, 1)$ is the evaporation rate
- $\Delta\tau_{ij}^k = Q / L_k$ if agent $k$ traversed edge $(i, j)$ in its solution path, proportional to inverse path cost $L_k$; else $0$
- $Q$ is a normalization constant

**Edge selection probability** for agent $k$ choosing the next sub-task from task $i$:

$$p_{ij}^k = \frac{[\tau_{ij}]^\alpha \cdot [\eta_{ij}]^\beta}{\sum_{l \in \text{feasible}(i)} [\tau_{il}]^\alpha \cdot [\eta_{il}]^\beta}$$

Where:
- $\alpha$ controls pheromone influence (exploitation)
- $\beta$ controls heuristic influence (domain-specific bias)
- $\eta_{ij}$ is a heuristic desirability (e.g., estimated sub-task difficulty)

### 3.4 Morph Dynamics — Response Threshold Model

Following Bonabeau, Theraulaz, & Deneubourg (1996), each agent has a vector of response thresholds $\theta_i^c$ for each morph $c \in \{\text{explorer}, \text{worker}, \text{evaluator}, \text{swarm_coordinator}\}$.

The probability that agent $i$ switches to morph $c$ given stimulus $s_c$:

$$P(i \to c) = \frac{s_c^n}{s_c^n + (\theta_i^c)^n}$$

Where:
- $s_c$ is the global stimulus for morph $c$ (e.g., number of unexplored branches → explorer stimulus; number of pending tasks → worker stimulus)
- $n$ is a steepness parameter (typically $n = 2$)
- Individual thresholds $\theta_i^c$ create heterogeneity — not all agents switch at the same stimulus level

**Self-regulation:** As more agents adopt morph $c$, they reduce the stimulus $s_c$ (explorers reduce unexplored branches, workers reduce pending tasks). This negative feedback loop maintains a balanced morph distribution without a scheduler.

### 3.5 Composition — Vicsek × ACO × Morph = Swarm Intelligence

The three mechanisms compose orthogonally:

```
┌─────────────────────────────────────────────────────────────┐
│                    SwarmField (Vicsek)                       │
│  What: Global strategy alignment via local averaging         │
│  When: Every tick                                            │
│  Effect: Agents converge on promising strategy directions    │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              PheromoneGraph (ACO)                      │   │
│  │  What: Path marking through task decomposition space   │   │
│  │  When: After each agent completes a sub-task           │   │
│  │  Effect: Good decompositions attract more agents       │   │
│  │                                                        │   │
│  │  ┌──────────────────────────────────────────────┐     │   │
│  │  │          MorphScheduler (Colony)              │     │   │
│  │  │  What: Dynamic role assignment via thresholds  │     │   │
│  │  │  When: On morph stimulus change                │     │   │
│  │  │  Effect: Balanced workforce without scheduler  │     │   │
│  │  └──────────────────────────────────────────────┘     │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

**One tick of the swarm:**

1. **Morph check:** Each agent evaluates morph stimuli against its thresholds. May switch roles.
2. **Pheromone read:** Explorer agents read the pheromone graph to choose which branch to explore. Worker agents follow the highest-pheromone trails.
3. **Act:** Each agent performs one unit of work (explore a sub-task, execute a tool, evaluate a result).
4. **Pheromone write:** Agents that completed work deposit pheromone proportional to result quality.
5. **Align:** All agents update their strategy vectors using the weighted Vicsek rule.
6. **Evaporate:** Global pheromone decay applied to all edges.
7. **Convergence check:** Compute $\phi$. If above threshold for sustained window, declare convergence.

---

## 4. Architecture

### 4.1 Layer Diagram

```
┌──────────────────────────────────────────────────────────────────────┐
│                     celer::necto (RFC-004 + this RFC)                 │
│                                                                        │
│  ┌────────────────────────────────── RFC-004 core ──────────────────┐ │
│  │  ActorRef  Envelope  Mailbox  ActorContext  ActorVTable           │ │
│  │  ActorHandle  ActorSystem  tick() / drain() / inject()           │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                                                        │
│  ┌─────────────────────── necto::swarm (this RFC) ─────────────────┐ │
│  │                                                                  │ │
│  │  ┌─────────────┐  ┌──────────────┐  ┌────────────────────────┐  │ │
│  │  │ SwarmField   │  │ PheromoneGraph│  │ MorphScheduler         │  │ │
│  │  │              │  │              │  │                        │  │ │
│  │  │ heading[]    │  │ Graph<V,E>   │  │ thresholds[]           │  │ │
│  │  │ neighbors()  │  │ deposit()    │  │ stimulus[]             │  │ │
│  │  │ align()      │  │ evaporate()  │  │ evaluate()             │  │ │
│  │  │ order_param()│  │ select()     │  │ rebalance()            │  │ │
│  │  └──────────────┘  └──────────────┘  └────────────────────────┘  │ │
│  │                                                                  │ │
│  │  SwarmAgent : Actor    Swarm (extends ActorSystem tick phases)    │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │  celer::stream (RFC-002) — for pheromone graph scans             │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │  celer-mem (RFC-001) — persistent storage for trails/state       │ │
│  └──────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.2 Core Types

```
SwarmConfig         — population, η, R, ρ, morph thresholds, convergence criteria
AgentState          — heading vector, morph, quality score, position in task space
SwarmField          — N agents × d dimensions, Vicsek update, order parameter
PheromoneGraph      — DAG of task nodes with weighted edges, backed by celer-mem
PheromoneEdge       — {from, to, τ, heuristic_η, age}
MorphScheduler      — threshold vectors per agent, stimulus computation, rebalance
Morph               — enum {Explorer, Worker, Evaluator, SwarmCoordinator}
TaskNode            — {id, description, parent, children, depth, status}
TaskTree            — the emergent decomposition structure
Swarm               — top-level coordinator owning field + graph + scheduler + necto system
```

### 4.3 SwarmField — Vicsek Alignment Engine

```cpp
namespace celer::necto::swarm {

struct AgentState {
    std::vector<float> heading;    // unit vector in R^d — current strategy
    float              quality;    // estimated solution fitness [0, 1]
    Morph              morph;      // current role
    uint32_t           agent_id;   // maps to necto::ActorRef
};

class SwarmField {
    std::vector<AgentState> agents_;
    std::size_t             dim_;      // d — strategy space dimensionality
    float                   radius_;   // R — alignment neighborhood radius
    float                   eta_;      // η — noise amplitude
    std::mt19937            rng_;

public:
    explicit SwarmField(std::size_t population, std::size_t dim, float radius, float eta);

    /// One Vicsek alignment step — O(N²) naive, O(N log N) with spatial index
    void align();

    /// Compute global order parameter φ ∈ [0, 1]
    [[nodiscard]] auto order_parameter() const -> float;

    /// Inject noise burst (break premature convergence)
    void inject_noise(float burst_eta);

    /// Access agent state for read/mutation
    [[nodiscard]] auto agent(std::size_t i) -> AgentState&;
    [[nodiscard]] auto agent(std::size_t i) const -> const AgentState&;
    [[nodiscard]] auto size() const -> std::size_t;

    /// Find neighbors within radius R in strategy space
    [[nodiscard]] auto neighbors(std::size_t i) const -> std::vector<std::size_t>;
};

} // namespace celer::necto::swarm
```

**Complexity:**

| Operation | Naive | With kd-tree / VP-tree |
|-----------|-------|------------------------|
| `align()` | $O(N^2 d)$ | $O(N \log N \cdot d)$ |
| `neighbors(i)` | $O(N d)$ | $O(\log N \cdot d)$ |
| `order_parameter()` | $O(N d)$ | $O(N d)$ (no shortcut) |

For swarms up to ~1000 agents, the naive $O(N^2)$ is acceptable (1M dot products × d dimensions). Beyond 1000, a VP-tree (vantage-point tree) spatial index partitions strategy space for $O(\log N)$ neighbor queries. The VP-tree is rebuilt once per tick — $O(N \log N)$ amortized.

### 4.4 PheromoneGraph — Stigmergic Memory

```cpp
namespace celer::necto::swarm {

struct TaskNodeId {
    uint64_t id;
    [[nodiscard]] constexpr bool valid() const noexcept;
    constexpr bool operator==(const TaskNodeId&) const noexcept = default;
};

struct PheromoneEdge {
    TaskNodeId from;
    TaskNodeId to;
    float      tau;            // pheromone strength
    float      heuristic_eta;  // domain-specific desirability
    uint64_t   age;            // ticks since last deposit
};

struct TaskNode {
    TaskNodeId          id;
    std::string         description;
    TaskNodeId          parent;
    std::vector<TaskNodeId> children;
    uint32_t            depth;         // tier in the decomposition
    TaskStatus          status;        // {unexplored, in_progress, completed, failed}
};

class PheromoneGraph {
    celer::Store* store_;           // persistent backing store
    std::string   scope_;           // celer-mem scope for this graph
    float         rho_;             // evaporation rate
    float         Q_;               // deposit normalization constant
    float         alpha_;           // pheromone weight in selection
    float         beta_;            // heuristic weight in selection

public:
    PheromoneGraph(celer::Store& store, std::string scope, float rho,
                   float Q = 1.0f, float alpha = 1.0f, float beta = 2.0f);

    /// Add a new task node to the graph
    auto add_node(TaskNode node) -> VoidResult;

    /// Deposit pheromone along a solution path
    auto deposit(std::span<const TaskNodeId> path, float quality) -> VoidResult;

    /// Global evaporation step — decay all edges by factor (1 - ρ)
    auto evaporate() -> VoidResult;

    /// Probabilistic edge selection from a given node (ACO selection rule)
    [[nodiscard]] auto select_next(TaskNodeId from,
                                   std::span<const TaskNodeId> feasible,
                                   std::mt19937& rng) const -> TaskNodeId;

    /// Read the pheromone on a specific edge
    [[nodiscard]] auto read_edge(TaskNodeId from, TaskNodeId to) const
        -> Result<PheromoneEdge>;

    /// Stream all edges from a node — for large fan-outs
    [[nodiscard]] auto stream_edges(TaskNodeId from) const
        -> Result<celer::StreamHandle<PheromoneEdge>>;

    /// Get the full task tree (materialized)
    [[nodiscard]] auto task_tree() const -> Result<TaskTree>;

    /// Prune edges with τ below threshold (garbage collection)
    auto prune(float min_tau = 0.001f) -> VoidResult;
};

} // namespace celer::necto::swarm
```

**Storage layout in celer-mem:**

```
swarm/{graph_scope}/
├── nodes/
│   ├── {node_id} → TaskNode (msgpack)
│   └── ...
├── edges/
│   ├── {from_id}:{to_id} → PheromoneEdge (msgpack)
│   └── ...
└── meta/
    ├── tick_count → uint64
    ├── total_deposits → uint64
    └── pruned_edges → uint64
```

**Persistence matters:** Unlike volatile pheromone models in simulation, our trails persist across process restarts via celer-mem. An agent swarm can be checkpointed, restarted, and resume with its accumulated knowledge. This is critical for long-running tasks that span hours or days.

### 4.5 MorphScheduler — Division of Labor

```cpp
namespace celer::necto::swarm {

enum class Morph : uint8_t {
    Explorer,     // discovers new task branches, expands the decomposition tree
    Worker,       // executes known sub-tasks along high-pheromone trails
    Evaluator,    // validates completed work, computes quality scores
    SwarmCoordinator,        // meta-coordinator: monitors order parameter, triggers noise injection
};

struct MorphConfig {
    std::string name;
    float       base_threshold;    // θ_c — base response threshold
    float       stimulus_decay;    // how fast stimulus reduces when morph is populated
};

class MorphScheduler {
    struct AgentThresholds {
        float explorer;
        float worker;
        float evaluator;
        float swarm_coordinator;
    };

    std::vector<AgentThresholds> thresholds_;   // per-agent threshold vectors
    std::vector<Morph>           assignments_;  // current morph per agent
    std::array<float, 4>         stimuli_;      // global stimulus per morph
    float                        steepness_;    // n in the response function

public:
    explicit MorphScheduler(std::size_t population,
                            std::span<const MorphConfig> configs,
                            float steepness = 2.0f);

    /// Recompute stimuli from swarm state, probabilistically reassign morphs
    void evaluate(const SwarmField& field, const PheromoneGraph& graph,
                  std::mt19937& rng);

    /// Get current morph distribution
    [[nodiscard]] auto distribution() const -> std::array<std::size_t, 4>;

    /// Get an agent's current morph
    [[nodiscard]] auto morph_of(std::size_t agent_id) const -> Morph;

    /// Override an agent's thresholds (for seeded specialists)
    void set_thresholds(std::size_t agent_id, AgentThresholds t);

    /// Compute stimulus for a given morph from current swarm state
    [[nodiscard]] auto compute_stimulus(Morph m, const SwarmField& field,
                                        const PheromoneGraph& graph) const -> float;
};

} // namespace celer::necto::swarm
```

**Stimulus computation:**

| Morph | Stimulus Source | High Stimulus Means |
|-------|----------------|---------------------|
| Explorer | Unexplored branches / total branches | Many unknowns — need more exploration |
| Worker | Pending tasks / total tasks | Backlog building — need more execution |
| Evaluator | Completed-but-unevaluated / total completed | Quality bottleneck — need more validation |
| SwarmCoordinator | $1 - \phi$ (disorder) | Swarm is unfocused — need meta-coordination |

**Self-regulation example:** 10 unexplored branches → high explorer stimulus → agents with low explorer thresholds switch to Explorer → they explore branches → unexplored count drops → explorer stimulus drops → no more switching. The system finds its own equilibrium without a scheduler.

### 4.6 Swarm — The Top-Level Coordinator

```cpp
namespace celer::necto::swarm {

struct SwarmConfig {
    std::size_t population{64};
    std::size_t strategy_dim{16};       // d — embedding dimensionality
    float       alignment_radius{0.3f}; // R — Vicsek neighborhood
    float       noise_eta{0.1f};        // η — alignment noise
    float       evaporation_rate{0.05f};// ρ — pheromone decay
    float       convergence_phi{0.9f};  // φ* — convergence threshold
    std::size_t sustain_window{50};     // ticks φ must exceed φ* to declare convergence
    std::size_t max_rounds{10'000};

    float       premature_quality{0.3f};  // if φ > φ* but quality < this, inject noise
    float       noise_burst{0.5f};        // η during noise injection burst

    std::vector<MorphConfig> morphs;
};

struct SwarmResult {
    TaskTree                     solution;       // the emergent decomposition
    std::vector<SolutionPath>    ranked_trails;  // top-K pheromone trails
    std::size_t                  rounds;
    std::size_t                  phase_transitions;  // # times φ crossed φ*
    std::array<std::size_t, 4>   final_morph_dist;
    float                        final_phi;
    float                        best_quality;
};

class Swarm {
    SwarmConfig     cfg_;
    SwarmField      field_;
    PheromoneGraph  graph_;
    MorphScheduler  scheduler_;
    ActorSystem*   system_;  // borrowed — swarm agents live in this system
    celer::Store*  store_;   // borrowed — persistent backing store
    std::mt19937    rng_;

public:
    Swarm(SwarmConfig cfg, ActorSystem& system, celer::Store& store);

    /// Inject a new top-level task into the swarm
    auto inject_task(Task task) -> VoidResult;

    /// Run one tick of the swarm loop
    auto tick() -> VoidResult;

    /// Run until convergence or max_rounds
    auto run_to_convergence() -> Result<SwarmResult>;

    /// Snapshot current state for inspection/debugging
    [[nodiscard]] auto snapshot() const -> SwarmSnapshot;

    /// Access sub-components
    [[nodiscard]] auto field() -> SwarmField&;
    [[nodiscard]] auto graph() -> PheromoneGraph&;
    [[nodiscard]] auto scheduler() -> MorphScheduler&;
};

} // namespace celer::necto::swarm
```

### 4.7 Agent Actors — The Swarm Particles

Each agent in the swarm IS a `necto::Actor` — same concept, same vtable, same mailbox. The morph determines behavior:

```cpp
namespace celer::necto::swarm {

struct SwarmAgent {
    uint32_t       agent_id;
    SwarmField*    field;      // borrowed
    PheromoneGraph* graph;     // borrowed
    MorphScheduler* scheduler; // borrowed

    void on_receive(necto::Envelope env, necto::ActorContext& ctx) {
        auto msg = deserialize<SwarmMessage>(env.payload);

        switch (scheduler->morph_of(agent_id)) {
            case Morph::Explorer:  handle_explore(msg, ctx);  break;
            case Morph::Worker:    handle_work(msg, ctx);     break;
            case Morph::Evaluator: handle_evaluate(msg, ctx); break;
            case Morph::SwarmCoordinator:     handle_swarm_coordinator(msg, ctx);    break;
        }
    }

private:
    void handle_explore(const SwarmMessage& msg, necto::ActorContext& ctx) {
        // 1. Read pheromone graph — find unexplored branches
        // 2. Select branch via ACO probabilistic rule
        // 3. Attempt decomposition — generate sub-tasks
        // 4. Add new TaskNodes to graph
        // 5. Deposit pheromone on explored path
        // 6. Update own heading vector toward explored direction
        // 7. Broadcast discovery to neighbors
    }

    void handle_work(const SwarmMessage& msg, necto::ActorContext& ctx) {
        // 1. Read pheromone graph — find highest-pheromone executable task
        // 2. Execute the sub-task (tool invocation, code generation, etc.)
        // 3. Deposit pheromone proportional to execution quality
        // 4. Mark task as completed
        // 5. Send result to nearest Evaluator
    }

    void handle_evaluate(const SwarmMessage& msg, necto::ActorContext& ctx) {
        // 1. Receive completed work from Worker
        // 2. Run quality assessment (tests, validation, heuristic scoring)
        // 3. Update quality score in SwarmField
        // 4. Deposit/penalize pheromone based on evaluation result
        // 5. If quality below threshold, re-queue task as unexplored
    }

    void handle_swarm_coordinator(const SwarmMessage& msg, necto::ActorContext& ctx) {
        // 1. Read global order parameter φ
        // 2. If φ > φ* but quality < threshold → inject noise burst
        // 3. If morph imbalance detected → broadcast rebalance signal
        // 4. If convergence sustained → signal completion
        // 5. Periodic: trigger pheromone pruning
    }
};

} // namespace celer::necto::swarm
```

---

## 5. Zero-Shot Efficacy on Novel Tasks

### 5.1 Why Swarm Intelligence Achieves Zero-Shot

The key insight: **zero-shot in a swarm ≠ zero-shot in a single agent**.

A single agent facing a novel task must either:
1. Generalize from training (fragile — distribution shift breaks it)
2. Be explicitly programmed for the task (not zero-shot by definition)

A swarm achieves zero-shot through **emergent decomposition**:

1. **Random exploration phase:** Explorer agents try random decompositions of the novel task. Most fail. A few accidentally find useful sub-tasks.
2. **Pheromone amplification:** Successful decompositions leave pheromone. Other explorers are biased toward these sub-tasks.
3. **Vicsek alignment:** Agents in the neighborhood of successful agents align their strategies, creating a cluster of similar approaches.
4. **Phase transition:** Below the critical noise level, the cluster grows to encompass the majority of the swarm. A coherent solution emerges.
5. **Worker execution:** Workers follow the high-pheromone trails to execute the decomposition.
6. **Evaluator feedback:** Evaluators validate results, reinforcing good trails and penalizing bad ones.

No individual agent needs to "know" how to solve the novel task. The swarm discovers the solution structure through the same mechanism ants discover food sources they've never seen before — **positive feedback loops amplifying random successes**.

### 5.2 N-Tier Decomposition

For tasks requiring n levels of decomposition:

```
Tier 0: "Implement distributed rate limiter"
    │
    ├── Tier 1: "Design token bucket algorithm"
    │   ├── Tier 2: "Implement sliding window counter"
    │   │   ├── Tier 3: "Define Redis data structure"
    │   │   └── Tier 3: "Implement atomic increment"
    │   └── Tier 2: "Implement rate limit middleware"
    │       ├── Tier 3: "Parse rate limit headers"
    │       └── Tier 3: "Wire into HTTP pipeline"
    │
    ├── Tier 1: "Design cross-region consistency"
    │   ├── Tier 2: "Choose consistency model (eventual/strong)"
    │   │   └── Tier 3: "Implement CRDTs for counter sync"
    │   └── Tier 2: "Implement gossip protocol"
    │       ├── Tier 3: "Node discovery"
    │       └── Tier 3: "Anti-entropy reconciliation"
    │
    └── Tier 1: "Implement crash recovery"
        ├── Tier 2: "WAL for in-flight tokens"
        └── Tier 2: "Checkpoint and restore"
```

**The swarm discovers this tree, not a programmer.** Explorers try random decompositions at Tier 0. Successful splits deposit pheromone. Workers execute Tier 1 sub-tasks. Those that succeed trigger further exploration at Tier 2. The tree grows organically, with depth bounded only by terminal sub-tasks (tasks that can be directly executed without further decomposition).

**Depth control:** The `max_depth` parameter in `SwarmConfig` prevents unbounded recursion. At `max_depth`, Explorer agents stop decomposing and Workers must attempt direct execution.

### 5.3 Handling Failure and Backtracking

Unlike DAG schedulers, the swarm handles failure gracefully:

1. **Failed execution:** Worker executes a sub-task → failure → negative pheromone deposit (trail weakening). Future workers avoid this path.
2. **Failed decomposition:** Explorer proposes sub-tasks that all fail → parent trail weakens. Explorers try different decompositions of the same parent.
3. **Dead end:** All children of a node fail → the node accumulates negative pheromone → eventually pruned by `PheromoneGraph::prune()`. The swarm abandons that entire subtree.
4. **Premature convergence:** Swarm aligns on a bad strategy → SwarmCoordinator detects high $\phi$ but low quality → noise burst → $\phi$ drops → exploration restarts from a new random direction.

This is **annealing-like behavior**: the noise parameter $\eta$ acts like temperature. High noise = exploration. Low noise = exploitation. The SwarmCoordinator modulates $\eta$ based on convergence quality, implementing a biological version of simulated annealing.

---

## 6. Invariants

| # | Invariant | Mechanism |
|---|-----------|-----------|
| 1 | **No central planner** | All coordination emerges from local rules. SwarmField, PheromoneGraph, and MorphScheduler operate on local neighborhoods. |
| 2 | **Monotonic pheromone identity** | TaskNodeId is monotonically increasing uint64. No reuse, no ambiguity. |
| 3 | **Pheromone bounded** | Evaporation + pruning prevent unbounded growth. τ ∈ [min_tau, max_tau] with clamping. |
| 4 | **Morph conservation** | Total agents = sum of all morph counts. No agent is unassigned. |
| 5 | **Order parameter bounded** | $\phi \in [0, 1]$ by construction (magnitude of mean unit vector). |
| 6 | **Noise injection is temporary** | Burst noise decays back to base $\eta$ over a configured window. |
| 7 | **Solution paths are DAGs** | PheromoneGraph enforces parent-child depth ordering. No cycles. |
| 8 | **Persistent trails** | Pheromone graph survives process restart via celer-mem backend. |
| 9 | **Actor isolation preserved** | Each SwarmAgent is a necto::Actor — no shared mutable state between agents. |
| 10 | **Tick determinism** | Given same RNG seed and initial state, the swarm produces identical results. |

---

## 7. Thread Safety

| Component | Safety | Mechanism |
|-----------|--------|-----------|
| `SwarmField::agents_` | Single-writer | Only mutated during `align()`, called in tick loop |
| `PheromoneGraph` | Multi-reader, single-writer | Workers read concurrently; deposits/evaporation batch in tick |
| `MorphScheduler` | Single-writer | Only mutated during `evaluate()`, called in tick loop |
| `SwarmAgent` | Isolated | Each agent is a necto::Actor with no shared state |
| `Swarm::tick()` | NOT re-entrant | Sequential phases within a tick: morph → read → act → write → align → evaporate |
| `TaskTree` (read snapshot) | Thread-safe | Okasaki-immutable snapshot via celer-mem's composite tree |

**Parallelism model:** Same as necto (RFC-004 §5) — the tick loop is single-threaded. For parallel swarms, run multiple `Swarm` instances and exchange top-K trails between them (island model). The pheromone graph's celer-mem storage is backend-safe for concurrent readers.

---

## 8. Performance

### 8.1 Operation Costs

| Operation | Cost |
|-----------|------|
| `align()` — naive | $O(N^2 d)$ where $N$ = population, $d$ = strategy dim |
| `align()` — VP-tree | $O(N \log N \cdot d)$ |
| `order_parameter()` | $O(Nd)$ |
| `evaporate()` | $O(\|E\|)$ — linear in graph edge count |
| `deposit()` | $O(\|P\|)$ — linear in path length |
| `select_next()` | $O(\|F\|)$ — linear in feasible edges from node |
| `evaluate()` (morph) | $O(N \cdot C)$ — N agents × C morphs |
| Full tick | $O(N^2 d + \|E\| + N \cdot C)$ naive; $O(N \log N \cdot d + \|E\| + NC)$ optimized |

### 8.2 Memory

| Component | Cost |
|-----------|------|
| SwarmField | $N \times (d \times 4 + 12)$ bytes — heading float[d] + quality + morph + id |
| PheromoneGraph (hot) | In-memory edge cache: $\|E\| \times 28$ bytes per edge |
| PheromoneGraph (cold) | On-disk via celer-mem — bounded by backend |
| MorphScheduler | $N \times (4 \times 4 + 1)$ bytes — 4 thresholds + assignment |
| Per SwarmAgent (necto) | ~176 bytes (ActorHandle overhead, §RFC-004 §6.2) |

### 8.3 Target Benchmarks

| Benchmark | Target |
|-----------|--------|
| 64 agents, 16-dim, 1 tick (naive) | < 100μs |
| 64 agents, 16-dim, full convergence (~500 ticks) | < 50ms |
| 256 agents, 32-dim, 1 tick (VP-tree) | < 500μs |
| Pheromone deposit (10-node path) | < 50μs |
| Pheromone evaporate (1000 edges) | < 200μs |
| Morph evaluate (256 agents) | < 50μs |
| Full convergence, 256 agents, tier-3 task | < 500ms |

---

## 9. Demonstration: Zero-Shot Task Decomposition

### 9.1 A Novel Task the Swarm Has Never Seen

```cpp
#include <celer/necto/swarm/swarm.hpp>
#include <celer/celer.hpp>
#include <celer/necto/actor.hpp>

int main() {
    // Set up celer-mem backing store
    celer::Store store{"./swarm.db", celer::sqlite_backend{}};
    necto::ActorSystem sys;

    // Configure the swarm — same system the agents live in
    necto::swarm::SwarmConfig cfg{
        .population       = 64,
        .strategy_dim     = 16,
        .alignment_radius = 0.3f,
        .noise_eta        = 0.15f,
        .evaporation_rate = 0.05f,
        .convergence_phi  = 0.9f,
        .sustain_window   = 50,
        .max_rounds       = 5000,
        .premature_quality = 0.3f,
        .noise_burst      = 0.5f,
        .morphs = {
            {"explorer",  0.8f, 0.1f},
            {"worker",    0.4f, 0.1f},
            {"evaluator", 0.6f, 0.1f},
            {"swarm_coordinator",     0.95f, 0.05f},
        },
    };

    necto::swarm::Swarm swarm(cfg, sys, store);

    // Inject a completely novel task — no prior trails exist
    swarm.inject_task({
        .description = "Build a real-time collaborative text editor with OT/CRDT",
        .constraints = {"< 100ms sync latency", "offline-first", "conflict-free"},
        .tier = necto::swarm::TaskTier::N4,
    });

    // The swarm has never seen this task. Watch it self-organize.
    auto result = swarm.run_to_convergence();

    if (result) {
        fmt::print("Converged in {} rounds\n", result->rounds);
        fmt::print("Phase transitions: {}\n", result->phase_transitions);
        fmt::print("Final φ: {:.3f}\n", result->final_phi);
        fmt::print("Best quality: {:.3f}\n", result->best_quality);
        fmt::print("Morph distribution: E={} W={} V={} Q={}\n",
            result->final_morph_dist[0], result->final_morph_dist[1],
            result->final_morph_dist[2], result->final_morph_dist[3]);

        // Print the emergent task decomposition
        print_task_tree(result->solution);
        // Tier 0: "Build real-time collaborative text editor with OT/CRDT"
        //   Tier 1: "Implement CRDT data structure"
        //     Tier 2: "Choose CRDT type (RGA, LSEQ, Yjs)"
        //       Tier 3: "Implement RGA insert/delete operations"
        //       Tier 3: "Implement vector clock for causality"
        //     Tier 2: "Implement merge function"
        //       Tier 3: "Define partial order on operations"
        //       Tier 3: "Implement conflict resolution rules"
        //   Tier 1: "Build sync protocol"
        //     Tier 2: "WebSocket transport layer"
        //     Tier 2: "Delta-state sync optimization"
        //   Tier 1: "Implement offline-first storage"
        //     Tier 2: "IndexedDB persistence layer"
        //     Tier 2: "Operation log with compaction"
        //   ... (emergent decomposition — varies per run)
    }
}
```

### 9.2 Convergence Trace (Simulated)

```
Tick    φ       Explorers  Workers  Evaluators  SCoords  Active Edges  Quality
----    -----   ---------  -------  ----------  -------  -----------   -------
0       0.041   50         10       3           1       0             0.000
50      0.087   45         12       5           2       34            0.052
100     0.153   38         16       8           2       89            0.121
200     0.312   28         22       11          3       187           0.234
300     0.487   20         28       13          3       312           0.378
350     0.891   8          38       15          3       401           0.289 ← premature!
351     0.341   35         18       8           3       401           0.289 ← noise burst
400     0.198   40         14       7           3       456           0.312
500     0.423   25         24       12          3       534           0.445
700     0.712   12         34       15          3       612           0.623
900     0.934   5          40       16          3       678           0.812 ← true convergence
950     0.961   4          42       15          3       695           0.867
1000    0.972   3          43       15          3       701           0.891 ← sustained ✓
```

**Key observations:**
- At tick 350, φ exceeds threshold but quality is only 0.289 → SwarmCoordinator injects noise, breaking false consensus
- After noise burst, swarm re-explores and finds a better decomposition
- True convergence at tick 900: φ > 0.9 AND quality > 0.8
- Sustained for 50+ ticks → declared converged at tick ~1000
- Final morph distribution naturally settled: few explorers (little left to explore), many workers (executing the plan)

### 9.3 Island Model — Parallel Swarms for Harder Problems

For tasks beyond the capacity of a single swarm, run multiple swarms in parallel (island model):

```cpp
// Launch 4 island swarms, each with 64 agents
std::vector<necto::swarm::Swarm> islands;
for (int i = 0; i < 4; ++i) {
    auto cfg_i = cfg;
    cfg_i.noise_eta = 0.1f + i * 0.05f;  // different noise regimes per island
    islands.emplace_back(cfg_i, systems[i], stores[i]);
    islands.back().inject_task(task);
}

// Run islands in parallel (each on its own thread)
std::vector<std::future<necto::swarm::SwarmResult>> futures;
for (auto& island : islands) {
    futures.push_back(std::async([&]{ return island.run_to_convergence(); }));
}

// Migration: every 100 ticks, exchange top-K trails between islands
// (Application-level — not built into the core)

// Select best result across islands
auto best = select_best(futures);
```

---

## 10. Comparison to Existing Approaches

| Property | Traditional DAG | Supervisor Tree | **Vicsek Swarm (this RFC)** |
|----------|----------------|-----------------|----------------------------|
| Task decomposition | Pre-programmed | Top-down from supervisor | Emergent from exploration |
| Novel task handling | Fails (no DAG) | Fails (no plan) | Succeeds (random search + amplification) |
| Scalability | O(nodes in DAG) | O(tree depth) | O(N² → N log N) agents |
| Failure recovery | Re-run failed node | Supervisor restarts | Pheromone decay (organic) |
| Load balancing | Static assignment | Supervisor decides | Morph self-regulation |
| Adaptability | None (static) | Limited (predefined strategies) | Continuous (noise + evaporation) |
| Convergence guarantee | Deterministic | Deterministic | Probabilistic (phase transition) |
| Information sharing | Explicit passing | Up-down the tree | Stigmergy (environment-mediated) |
| Role assignment | Fixed | Fixed hierarchy | Dynamic (response thresholds) |
| Central point of failure | DAG executor | Supervisor | None (fully distributed) |

---

## 11. Open Questions and Future Work

### 11.1 Open Research Questions

1. **Optimal $\eta$ scheduling:** Is there an adaptive noise schedule that provably converges faster than constant $\eta$? The SwarmCoordinator's noise injection is heuristic — is there a principled annealing schedule?

2. **Strategy space dimensionality:** What is the optimal $d$ for a given task complexity? Too low → insufficient expressiveness. Too high → curse of dimensionality in neighbor search.

3. **Pheromone vs. Vicsek weight balance:** The $\alpha / \beta$ balance in ACO selection is task-dependent. Can it be auto-tuned during the exploration phase?

4. **Convergence bounds:** For a task of tier $n$ with branching factor $b$, what is the expected number of ticks to convergence as a function of $N$, $\eta$, $R$, $\rho$?

5. **Quality function design:** The quality score determines what the swarm converges *on*. An improperly designed quality function causes convergence on bad solutions. How do we validate quality functions without domain expertise?

### 11.2 Future Extensions (Out of Scope for v1)

| Extension | Description | Depends On |
|-----------|-------------|------------|
| **Multi-node swarm** | Swarms across machines, pheromone replication via celer-mem S3 backend | RFC-002 (S3), cluster consensus |
| **Persistent swarm memory** | Swarm that remembers previous tasks and reuses trail patterns | Pheromone graph archival + similarity matching |
| **Dynamic population** | Spawn/kill agents based on task complexity signal | Adaptive N |
| **Parallel tick** | Multi-threaded tick with lane partitioning | necto lane-parallelism (RFC-004 future) |
| **LLM integration** | Explorer agents use LLM for sub-task generation; Worker agents use LLM for execution | External LLM API integration |
| **Task embedding** | Automatic strategy space embedding from task descriptions | Embedding model integration |
| **Visual debugger** | Real-time φ/morph/pheromone visualization | WebSocket + frontend (separate project) |

### 11.3 Theoretical Connections

| Theory | Connection to This Work |
|--------|------------------------|
| **Vicsek model** (1995) | Direct — alignment dynamics for strategy consensus |
| **Ant Colony Optimization** (Dorigo, 1996) | Direct — pheromone trail formation for path optimization |
| **Response Threshold Model** (Bonabeau, 1996) | Direct — morph self-regulation |
| **Simulated Annealing** (Kirkpatrick, 1983) | Analogous — noise parameter acts as temperature |
| **Particle Swarm Optimization** (Kennedy & Eberhart, 1995) | Related — but PSO uses global/personal best, Vicsek uses local alignment |
| **Stigmergy** (Grassé, 1959) | Foundational — environment as communication medium |
| **Self-Organized Criticality** (Bak, 1987) | The swarm operates near the phase transition boundary — maximizing both order and adaptability |
| **Renormalization Group** (Wilson, 1975) | N-tier decomposition is a natural renormalization — each tier coarse-grains the problem at a different scale |

---

## 12. Build System Changes

### 12.1 New Files

```
include/celer/necto/swarm/
├── field.hpp         # SwarmField — Vicsek alignment engine
├── pheromone.hpp     # PheromoneGraph — stigmergic trail memory
├── morph.hpp         # MorphScheduler — division of labor
├── swarm.hpp         # Swarm — top-level coordinator
└── types.hpp         # TaskNode, TaskTree, Morph enum, configs

src/necto/swarm/
├── field.cpp         # SwarmField implementation
├── pheromone.cpp     # PheromoneGraph implementation (celer-mem backed)
├── morph.cpp         # MorphScheduler implementation
└── swarm.cpp         # Swarm tick/drain loop

tests/
├── test_swarm_field.cpp      # Vicsek alignment + order parameter tests
├── test_pheromone.cpp        # ACO deposit/evaporate/select/prune tests
├── test_morph.cpp            # Morph stimulus/response/rebalance tests
├── test_swarm_integration.cpp # Full swarm convergence on known tasks
└── test_swarm_zeroshot.cpp   # Zero-shot decomposition of novel tasks

examples/
└── 06_swarm_decomposition.cpp  # Demo: inject novel task, observe convergence
```

### 12.2 Dependencies

No new external dependencies. Everything builds on:
- celer-mem core (RFC-001) — storage
- celer::stream (RFC-002) — pheromone graph streaming scans
- celer::necto core (RFC-004) — actor system that this RFC extends

Standard library: `<random>`, `<cmath>`, `<algorithm>`, `<numeric>`, `<array>`.

Optional (performance): VP-tree spatial index — header-only, bundled.

### 12.3 Compilation

```makefile
# Added to existing Makefile
SWARM_SRC = src/necto/swarm/field.cpp src/necto/swarm/pheromone.cpp \
            src/necto/swarm/morph.cpp src/necto/swarm/swarm.cpp
SWARM_OBJ = $(SWARM_SRC:.cpp=.o)
SWARM_TEST_SRC = tests/test_swarm_field.cpp tests/test_pheromone.cpp \
                 tests/test_morph.cpp tests/test_swarm_integration.cpp

celer_swarm_tests: $(SWARM_TEST_SRC) $(SWARM_OBJ) $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
```

---

## 13. References

1. Vicsek, T., Czirók, A., Ben-Jacob, E., Cohen, I., & Shochet, O. (1995). "Novel type of phase transition in a system of self-driven particles." *Physical Review Letters*, 75(6), 1226.

2. Dorigo, M., Maniezzo, V., & Colorni, A. (1996). "Ant system: optimization by a colony of cooperating agents." *IEEE Transactions on Systems, Man, and Cybernetics*, 26(1), 29-41.

3. Dorigo, M., & Stützle, T. (2004). *Ant Colony Optimization*. MIT Press.

4. Bonabeau, E., Theraulaz, G., & Deneubourg, J.-L. (1996). "Quantitative study of the fixed threshold model for the regulation of division of labour in insect societies." *Proceedings of the Royal Society B*, 263(1376), 1565-1569.

5. Hölldobler, B., & Wilson, E. O. (1990). *The Ants*. Harvard University Press.

6. Grassé, P.-P. (1959). "La reconstruction du nid et les coordinations interindividuelles chez *Bellicositermes natalensis* et *Cubitermes sp*." *Insectes Sociaux*, 6(1), 41-80.

7. Kennedy, J., & Eberhart, R. (1995). "Particle swarm optimization." *Proceedings of ICNN'95*, 4, 1942-1948.

8. Bak, P., Tang, C., & Wiesenfeld, K. (1987). "Self-organized criticality: An explanation of 1/f noise." *Physical Review Letters*, 59(4), 381.

9. Kirkpatrick, S., Gelatt, C. D., & Vecchi, M. P. (1983). "Optimization by simulated annealing." *Science*, 220(4598), 671-680.

10. Wilson, K. G. (1975). "The renormalization group: Critical phenomena and the Kondo problem." *Reviews of Modern Physics*, 47(4), 773.

11. Reynolds, C. W. (1987). "Flocks, herds and schools: A distributed behavioral model." *ACM SIGGRAPH Computer Graphics*, 21(4), 25-34.

12. Toner, J., & Tu, Y. (1995). "Long-range order in a two-dimensional dynamical XY model: How birds fly together." *Physical Review Letters*, 75(23), 4326.

---

## 14. Appendix A: Phase Transition Analysis

### A.1 Critical Noise Threshold

For the Vicsek model with $N$ particles in a box of size $L$, the critical noise threshold scales as:

$$\eta_c \propto \left(\frac{N \pi R^2}{L^2}\right)^{0.45}$$

In our application, $L^2$ is replaced by the volume of strategy space. For $d$-dimensional unit vectors on $S^{d-1}$, the effective density is:

$$\rho_{\text{eff}} = \frac{N \cdot V_d(R)}{A_d}$$

Where $V_d(R)$ is the volume of a $d$-dimensional ball of radius $R$ and $A_d$ is the surface area of $S^{d-1}$.

**Practical implication:** Higher dimensionality $d$ requires more agents $N$ or larger radius $R$ to maintain the same effective density above the critical transition. The `SwarmConfig` should be parameterized such that:

$$N \cdot V_d(R) > C_{\text{crit}} \cdot A_d$$

Where $C_{\text{crit}} \approx 4.51$ (empirically determined for Vicsek transition in continuum limit).

### A.2 Convergence Time Scaling

Numerical studies of the Vicsek model show convergence time scales as:

$$T_{\text{conv}} \propto \frac{L^2}{v_0 R} \cdot f(\eta / \eta_c)$$

Where $f$ diverges as $\eta \to \eta_c$ from below (critical slowing down). In our setting, $v_0 = 1$ (agents take unit steps in strategy space per tick) and $L/R$ is the ratio of strategy space diameter to alignment radius.

**Practical recommendation:** Set $R$ to cover at least $\sqrt{N}$ agents on average. For $N = 64$, $R \geq 0.25$ in normalized strategy space. For $N = 256$, $R \geq 0.15$. These values keep the system above the percolation threshold where information can propagate end-to-end in $O(\text{diameter} / R)$ ticks.

---

## 15. Appendix B: Comparison to Particle Swarm Optimization (PSO)

PSO (Kennedy & Eberhart, 1995) is superficially similar but fundamentally different:

| Property | PSO | Vicsek Swarm (this RFC) |
|----------|-----|------------------------|
| Information sharing | Global best + personal best | Local neighborhood averaging |
| Convergence mechanism | Attraction to known optima | Phase transition (emergent alignment) |
| Exploration mechanism | Inertia + random perturbation | Noise parameter η |
| Memory | Position + velocity per particle | Pheromone graph (stigmergy) |
| Division of labor | None (all particles equal) | Morph system (explorer/worker/evaluator/swarm_coordinator) |
| Applicable to | Continuous optimization | Combinatorial/structural task decomposition |
| Novel task handling | Requires objective function | Zero-shot via emergent decomposition |

**PSO converges to a point in search space. The Vicsek swarm converges to a structure (task tree).** This is the critical distinction — PSO optimizes parameters, our swarm discovers architectures.

---

## 16. Appendix C: Glossary

| Term | Definition |
|------|-----------|
| **Alignment** | The Vicsek process of averaging heading vectors within a local neighborhood |
| **Morph** | A behavioral role (Explorer, Worker, Evaluator, SwarmCoordinator) dynamically assigned via response thresholds |
| **Convergence** | Sustained order parameter $\phi > \phi^*$ with adequate quality score |
| **Evaporation** | Global pheromone decay that prevents trail lock-in and enables adaptation |
| **Heading vector** | A unit vector in $\mathbb{R}^d$ representing an agent's current task-strategy direction |
| **Island model** | Parallel swarm execution with periodic trail migration between islands |
| **Noise injection** | Temporary increase in $\eta$ to break premature convergence |
| **Order parameter** | $\phi \in [0, 1]$ — magnitude of the mean heading vector, measuring global alignment |
| **Pheromone** | Weighted trail on graph edges, encoding historical success of solution paths |
| **Phase transition** | The critical noise threshold $\eta_c$ below which the swarm spontaneously aligns |
| **Response threshold** | Per-agent, per-morph sensitivity to stimulus; determines when an agent switches roles |
| **Stigmergy** | Coordination through modification of a shared environment (pheromone graph) |
| **Strategy space** | The $d$-dimensional space in which agent heading vectors live |
| **Tier** | Depth level in the task decomposition tree. Tier 0 = root task. |
| **VP-tree** | Vantage-point tree — spatial index for efficient neighbor queries in arbitrary metric spaces |
| **Zero-shot** | Solving a task never previously encountered, without task-specific training or programming |
