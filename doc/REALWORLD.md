# Real-world suite: choosing the applications

**Aim:** show that software people widely use today gets faster with Ni, measured as they use it. pyperformance and the adversarial suite (`tools/adversarial/`) are for us; this suite is for everyone else, so the choice of applications must be defensible before any result is seen.

**When:** the suite runs on CPython 3.10 or later, once Ni works there, because today's versions of the important packages no longer support 3.7. The research below doesn't need Ni and can be done at any time. The list is chosen and the workloads are committed before Ni runs on any of them.

## What makes an application worth including

1. **Widely used now.** Evidence, not taste:
   - PyPI downloads (the public BigQuery dataset, pypistats);
   - dependents on GitHub;
   - the PSF/JetBrains Python Developers Survey (what people use Python for);
   - known production users.
2. **Its time is spent in Python bytecode, as shipped.** Many popular tools now ship compiled code: mypy and black are built with mypyc; ruff, pydantic-core, polars, orjson and tokenizers are Rust. Ni can't speed those up, and measuring a pure-Python build that nobody installs would be flattery. Each candidate is profiled under plain CPython to get the share of its time in the eval loop.
3. **A workload that matches real use:** a request through the whole web stack, a linter over a real codebase, a test suite run, a model served one request at a time. Take it from the project's own benchmarks or documentation where one exists.
4. **Reproducible:** pinned versions, inputs and models cached, runs offline, and output that can be checked against CPython's.
5. **Licences** that allow the inputs to be redistributed, or a script that fetches them.

## Candidates to research

These are starting points, not choices.

| Area | Candidates |
|---|---|
| Web | Django (Instagram runs it at scale), FastAPI/Starlette with uvicorn, Flask with Jinja2, aiohttp, gunicorn |
| Cloud and infrastructure | boto3/botocore (among the most downloaded packages), requests/urllib3, Ansible, Celery |
| Developer tools | pytest running a real suite, pylint/astroid, Sphinx building docs, pip resolving and installing, coverage.py |
| Data | pandas with Python callbacks, scikit-learn's Python-side pipelines, SQLAlchemy ORM, networkx, sympy |
| ML serving | Hugging Face transformers inference (small models, batch 1, CPU and GPU), the Python overhead per token in `generate()`, data loading and preprocessing |

**Controls:** two or three C-heavy workloads (numpy maths, large-model inference) where the expected result is no change. They show Ni does no harm where it can't help.

## Prior art to read first

- **Pyston's macrobenchmarks** (`pyston/python-macrobenchmarks`): flask blogging, djangocms, mypy, pylint, pycparser, PyTorch inference, gunicorn, aiohttp. These are the closest existing answer to this question; take what fits.
- **The Faster CPython team's choices:** pyperformance's application benchmarks and their reasons.
- **PyPy's speed centre:** where PyPy's real-world gains held up and where they didn't, and why (often C extensions, through `cpyext`).
- **Cinder:** Instagram's published Django workload.

## Steps

1. **Long list** (about 30) from the usage data, each with its evidence.
2. **Profile** each under plain CPython on its candidate workload: the share of time in the eval loop (`perf` with Python frames, or py-spy). Drop those below about a third unless they're controls.
3. **Score** each on importance, then share of time in Python, then reproducibility. Choose about 12, spanning the areas above, plus the controls.
4. **Write the result here:** each choice with its evidence, workload and profile, and each strong candidate left out with its reason.
5. **Commit the workloads and harness** (reusing `tools/adversarial/run.py`'s measurements: total time with start-up, warm-up, steady state, peak memory, output checked), blind, before Ni runs on them.
6. **First run on 3.10+, reported in full:** losses as prominently as wins.
