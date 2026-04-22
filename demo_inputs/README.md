# Demo Inputs — Agent Profiles

Each JSON file in this directory defines a simulated agent participating in a
multi-discipline design collaboration for a **civic annex building project**.
The agents are used by the e2e demo and benchmark harnesses to generate
realistic semantic lock traffic.

## JSON schema

```jsonc
{
  "agent_name": "Ari",
  "role": "Sustainability-Focused Design Agent",
  "personality": "...",           // flavour text; not sent to the lock service
  "objective": "...",             // flavour text; not sent to the lock service
  "payload": "...",               // matches first payload_schedule entry
  "scheduled_offset_ms": 0,      // matches first payload_schedule entry
  "operation": "write",           // "write" or "read"
  "payload_schedule": [           // ordered list of operations the agent performs
    {
      "scheduled_offset_ms": 0,   // delay before this operation fires (ms)
      "operation": "write",       // "write" upserts to Qdrant; "read" queries it
      "payload": "..."            // natural-language text → embedded → compared via cosine
    }
  ]
}
```

The `payload` text is embedded at runtime and compared against active locks
using cosine similarity.  When similarity ≥ θ (the configured threshold), the
request blocks until the conflicting lock is released.

## Agent roster

| Label | Name    | Discipline                      | Op pattern          |
|-------|---------|---------------------------------|---------------------|
| A     | Ari     | Sustainability / Energy         | write → write → read |
| B     | Brooke  | Fire / Life Safety & Code       | read → read → write  |
| C     | Casey   | Cost & Budget Control           | read → read → read   |
| D     | Devon   | Construction Logistics          | write → write → write|
| E     | Emerson | Client Experience / Program     | read → read → read   |
| F     | Frankie | Structural Engineering          | write → write → read |
| G     | Gray    | MEP Coordination                | read → write → write |
| H     | Harper  | Facade Engineering              | read → write → write |
| I     | Ira     | BIM Coordination                | read → read → write  |
| J     | Jordan  | Landscape & Urban Design        | write → read → write |
| K     | Kai     | Interior Architecture           | read → write → write |
| L     | Lane    | Geotechnical & Civil            | write → read → write |
| M     | Morgan  | Commissioning & QA              | read → read → write  |

## Intentional payload overlaps

Several agents share identical or near-identical payloads to model realistic
cross-discipline coordination where multiple roles work on the same design
artifact:

- **A ↔ B ↔ G ↔ M** — Massing concept / passive cooling for the civic annex.
  Ari writes it; Brooke, Gray, and Morgan read it.
- **A ↔ H** — Facade solar heat gain and shading strategies.
  Ari writes about it; Harper reads a close paraphrase.
- **D ↔ E ↔ L** — Construction phasing for the atrium steel package.
  Devon writes it; Emerson and Lane read it.
- **E ↔ K** — Meeting-room mix and occupancy profiles.
  Emerson reads it; Kai also reads the same text before designing interiors.
- **C ↔ F** — Structural frame options discussed from cost vs. engineering
  angles (moderate overlap, likely below θ).
- **C ↔ H** — Curtain wall systems discussed from cost vs. performance
  angles (moderate overlap, likely below θ).

Agents like **Jordan (J)** and **Ira (I)** are intentionally distant from most
others, demonstrating realistic parallelism where unrelated disciplines
proceed without blocking.

## Adding new profiles

Create a new `<LABEL>.json` following the schema above. The benchmark runner
in `src/benchmark_runner.cpp` and the demo harness in `src/e2e_demo.cpp`
currently iterate over a hardcoded label set (`"ABCDE"`) — update those
loops to include new labels.
