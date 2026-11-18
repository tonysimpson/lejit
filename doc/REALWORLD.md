# Real-world suite: the applications

**Aim:** show that software people widely use today gets faster with Ni, measured as they use it. pyperformance and the adversarial suite (`tools/adversarial/`) are for us; this suite is for everyone else, so the list below was chosen from usage data and plain-CPython profiles only, before Ni ran on any of it.

**When:** the suite runs on CPython 3.10 or later, once Ni works there; today's versions of these packages no longer support 3.7. The choice is made (steps 1–4); the workloads and harness (step 5) are committed blind before Ni runs on them.

## Criteria

1. **Widely used now**, by evidence: PyPI downloads, the PSF/JetBrains survey, known production users.
2. **Time spent in Python bytecode, as shipped.** Each candidate was profiled under plain CPython at the versions `pip install` gives today. Below about a third, it's a control or it's out.
3. **A workload that matches real use**: a request through the whole stack, a tool over a real codebase, a real test suite.
4. **Reproducible**: pinned versions, inputs cached, offline, output checked against CPython's.
5. **Licences** that let the inputs be redistributed, or a script that fetches them.

Then score on importance first, then share in Python, then reproducibility.

## Evidence used

- **Downloads** [read]: the 30-day top-PyPI-packages list (hugovk.dev, snapshot of 2026-11-01). Downloads mostly count CI and dependency installs, not deployments: boto3 is 1st and Django 466th.
- **PSF/JetBrains Python Developers Survey 2024** (published August 2025) [read]:
  - Main use: web 23%, ML 13%, data analysis 10%, data engineering 7%, DevOps 6%.
  - Web frameworks: FastAPI 38%, Django 35%, Flask 34%, aiohttp 13%.
  - Data and ML libraries: pandas 80%, NumPy 75%; among ML users, scikit-learn 68%, PyTorch 66%, Transformers 28%.
- **Production users** [read]: Instagram calls itself one of the largest Django deployments, served by gunicorn (engineering.fb.com, 2022). Other big-name claims found were second-hand and are not used.

## Prior art

- **Pyston's python-macrobenchmarks** [read]: aiohttp, djangocms, flaskblogging, gevent_hub, gunicorn, json, kinto, mypy2, pycparser, pylint, pytorch_alexnet_inference, thrift.
  - Its headline set is the three web-serving ones.
  - No reasons are stated.
  - Its mypy predates mypy being shipped compiled with mypyc.
- **pyperformance** [read]: its application benchmarks include django_template, sqlalchemy, sympy, sphinx, docutils, networkx, tornado_http, coverage, dask and yaml.
  - Rationale (faster-cpython/ideas#10): object-oriented code over numeric code, since numeric users already rely on numpy, and runs of minutes, not hours.
  - Most of these are components (a template engine on its own), not whole stacks.
- **PyPy's speed centre** [read]: its applications include django, sphinx, genshi, mako, sqlalchemy, sympy, twisted and html5lib.
  - PyPy's own performance page says its gains fade on runs under about 0.2 s (warm-up), on code that uses C extensions heavily (cpyext), and where its JIT can't help, where it is generally slower than CPython.
  - Lesson taken here: report start-up and warm-up, and include controls.
- **Cinder** [read]: Instagram has published no description of its Django workload, and no share of its time in Python. The only lesson is that a full Django request is the workload that matters to it.
- **Faster CPython's perf profiles** [read]: even pyperformance's application benchmarks spend about 28–48% in the interpreter proper, and the rest in lookups, allocation and GC.

## Method of the profiles

Everything below is **[run]** on CPython 3.12.9, the newest local CPython (pyenv; `-O3`, shared libpython, no frame pointers).

- **Setup:**
  - One venv in `build/realworld/` with current PyPI releases, e.g. Django 6.1.3, Flask 3.1.3, FastAPI 0.164, SQLAlchemy 2.1.9, pandas 3.1.1, botocore 1.44.5, torch 2.15 (CPU).
  - `OPENBLAS_NUM_THREADS=1`, because idle BLAS threads otherwise inflate the compiled share.
- **Sampling:** `perf record -e cycles:u --call-graph dwarf,65528` at 199 Hz.
  - Only user space is sampled. Measured system time was small, at most 8% on the servers.
- **Classification:** each sample belongs to its innermost owning frame:
  - **Py**: the eval loop, together with the runtime C it calls directly (calls, allocation, dict and attribute work). This is the part a JIT can affect.
  - **GC**: the cyclic collector.
  - **lib**: C library work called from bytecode (regex, str methods, hashing, I/O).
  - **C**: any extension or shared library (Cython, mypyc, Rust, numpy, torch, and stdlib C modules such as `_elementtree`).
  - What is left is unattributed: truncated stacks, the dynamic loader.
- **Runs:** one run each, including start-up and import, each a few seconds to 40 s long.
- **Servers:** run under `wrk -c1` for 15 s, and only the server is profiled.

## The suite

| # | Area | Application | Evidence | Workload | Py / GC / lib / C |
|---|---|---|---|---|---|
| 1 | Web | **Django** (gunicorn, sqlite) | Instagram; survey 35% | gunicorn sync worker; a view doing an ORM query plus a template with filters, through the default middleware | 81 / 2 / 8 / 9 |
| 2 | Web | **Flask + Jinja2** (gunicorn) | 195M/month (Jinja2 676M); survey 34%; Pyston's flaskblogging | gunicorn; a Jinja2 page with loops, filters and `url_for`, plus a JSON route | 83 / 1 / 7 / 1 |
| 3 | Web | **FastAPI** (uvicorn[standard]) | 507M/month; survey 38%, the top framework | uvicorn with uvloop and httptools, as `fastapi[standard]` installs it; pydantic models in and out | 72 / 0 / 5 / 22 |
| 4 | Cloud | **boto3/botocore** | #1 on PyPI (2.7B/month) | S3 `list_objects_v2` and DynamoDB `query`/`put_item`, with real serialising, SigV4 signing and parsing, but the HTTP send replaced by recorded responses | 78 / 2 / 6 / 14 |
| 5 | Infra | **Ansible** (ansible-core) | the main configuration-management tool, though only 16M/month | a playbook of templated `set_fact`/`assert`/`debug` loops on localhost | 82 / 2 / 3 / 3 |
| 6 | Infra | **PyYAML** | #12 (1.15B/month) | `safe_load_all`/`safe_dump_all` of Kubernetes-style manifests; the default loader is pure Python | 92 / 0 / 1 / 0 |
| 7 | Dev tools | **pytest** | #25 (926M/month) | pytest over `packaging`'s own suite (62k tests) | 77 / 14 / 3 / 0 |
| 8 | Dev tools | **pylint/astroid** | 41M/month; Pyston chose it | pylint over the `requests` source | 51 / 44 / 1 / 0 |
| 9 | Dev tools | **Sphinx** | 20M/month; the documentation tool; in pyperformance and PyPy's suite | HTML build of Flask's docs | 66 / 17 / 7 / 2 |
| 10 | Data | **SQLAlchemy ORM** | 395M/month | declarative models with relationships, 10k inserts, `selectinload` queries, updates (sqlite) | 73 / 17 / 1 / 8 |
| 11 | Data | **pandas with Python callbacks** | 669M/month; survey 80% | `read_csv`, `apply(axis=1)`, `map`, `groupby().apply`, merge, pivot | 53 / 3 / 1 / 33 |
| 12 | ML serving | **transformers `generate()`**, tiny model | 115M/month; survey 28% of ML users | greedy `generate()`, batch 1, 64 tokens, `hf-internal-testing/tiny-random-gpt2` on CPU: the Python cost per token | 39 / 17 / 7 / 23 |

**Controls**, where the expected result is no change:

| Control | Workload | Py / GC / lib / C |
|---|---|---|
| numpy maths | matmul, FFT, SVD, sort | 1 / 1 / 0 / 88 |
| small-model inference | the same `generate()` as #12 with `distilbert/distilgpt2`, 1 thread | 10 / 3 / 2 / 82 |
| vectorised pandas | groupby-agg, `np.where`, merge and sort on 2M rows | 4 / 2 / 1 / 92 |

Notes on the choices:

- **#3 FastAPI:** with plain `uvicorn` (pure-Python h11 and asyncio) its Py share is 85% [run]. The suite uses `[standard]` because that is what FastAPI's documentation installs.
- **#4 boto3/botocore:** the real network is left out on purpose. It is a wait, not CPU, and it can't run offline.
- **#5 Ansible:** the playbook uses only action plugins, so no module subprocesses are started. Real playbooks also start one Python process per module task, so a real run adds interpreter start-up per task [inferred]. Step 5 should include a few module tasks.
- **GC-heavy entries (#8, #7, #10):** Ni doesn't change the collector, so GC time bounds their gain [inferred].
- **#11 pandas** is mixed on purpose: it is the commonest data library, and callbacks are how people put Python into it.
- **#12 transformers:**
  - The tiny model shows a real cost, the Python per token in `generate()`. The model itself is a test fixture.
  - On GPU, that per-token Python would be a larger share [inferred]. This was not measured: only CPU torch was installed.
  - Import and model load take about 3.4 s of #12's run and are about half Python [run]. The harness should report them apart.

## Strong candidates left out

| Candidate | Evidence | Profile [run] | Reason |
|---|---|---|---|
| mypy | 199M/month | 11% Py; about 25% mypyc and Rust parser, the rest threads and unattributed | Ships compiled with mypyc; Pyston's benchmark predates that. |
| black | 126M/month | 7% Py, 80% mypyc | Ships compiled with mypyc. |
| ruff, orjson, polars, tokenizers | top downloads | not profiled | Rust; no Python to speed up. |
| pydantic on its own | #18 (1.07B/month) | 35% Py, 53% Rust core | Below the bar; the Python side of pydantic is already in #3. |
| aiohttp | 545M/month; survey 13%; Pyston | 76 / 1 / 3 / 20 (client and server in one process) | Strong. Left out because it overlaps #3's async stack and the suite is capped at 12. First reserve. |
| requests/urllib3 | #6 and #8 | 69% Py on the client; time also goes to sockets and the kernel | Wall time is mostly I/O; #4 covers building and parsing requests. |
| gunicorn | 118M/month | | It is the server in #1 and #2. |
| Celery | 51M/month; Instagram | about 78% Py on CPU, but wall time is broker polling | Needs a real broker (Redis or RabbitMQ) to be meaningful; none is available offline. Revisit with a local Redis. |
| pip resolving | universal | 49% Py; 1 s | Short. A real install is dominated by downloads, unpacking and subprocesses. |
| coverage.py | 329M/month | 72 / 4 / 5 / 18 (C tracer) under pytest | Measures Ni with a tracer active, which is a different question; on 3.12+ coverage can use `sys.monitoring`. Worth a separate study. |
| scikit-learn pipeline | 211M/month; survey 68% of ML users | 44 / 7 / 24 / 15 (text vectoriser, small fits) | The share depends heavily on the estimator; large fits are BLAS. Second reserve. |
| networkx, sympy | 244M and 153M/month | 87% Py each | Strong on profile, weaker on use than #10–#11, and already in pyperformance. Third reserve. |
| jsonschema, rich, beautifulsoup4, openpyxl | 600M, 546M, 316M, 338M/month | Py 78, 82, 70, 54 | Mostly pulled in by other packages; smaller standalone workloads. |
| docutils | 149M/month | 63 / 14 / 22 / 0 | Covered by #9. |
| Markdown, Pygments | 117M, 1.13B/month | Py 24 and 39; regex 69% and 60% | The regex engine dominates. |

## Caveats

- The profiles are from 3.12, not 3.10. 3.12's specialising interpreter makes the eval loop cheaper than 3.10's, so shares on 3.10 are likely a little higher [inferred].
- **Py** is an upper bound on what Ni can affect. It includes runtime C called from bytecode, which Ni removes only in part.
- Single runs at 199 Hz: shares are good to a few points. Runs under 2 s (#11, numpy) are coarser.
- Downloads overstate libraries that are installed in CI and pulled in as dependencies. The survey corrects for that only at the level of what people use Python for.

## Remaining steps

5. **Commit the workloads and harness, blind:**
   - Pin the versions above and cache the inputs: the sdists, the codebases, the wheels, and the Hugging Face models.
   - Reuse `tools/adversarial/run.py`'s measurements: total time with start-up, warm-up, steady state, peak memory, and output checked against CPython's.
   - The servers need a non-Python load generator (`wrk`) and a fixed request mix.
   - Ansible gains a few module tasks.
   - #12 reports load time and per-token time apart.
6. **First run on 3.10+, reported in full**, losses as prominently as wins, the controls included.
