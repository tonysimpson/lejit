"""Fetch the real-world suite's inputs into build/realworld/, checked by sha256.

    python fetch.py RW_DIR inputs        source archives (stdlib only)
    python fetch.py RW_DIR models        Hugging Face models, pinned by revision
                                         (run with the transformers venv's python)
"""
import hashlib, os, sys, tarfile, urllib.request

INPUTS = {
    # name: (url, sha256); unpacked to RW_DIR/inputs/<name>/
    'flask-3.1.3': ('https://files.pythonhosted.org/packages/26/00/35d85dcce6c57fdc871f3867d465d780f302a175ea360f62533f12b27e2b/flask-3.1.3.tar.gz',
                    '0ef0e52b8a9cd932855379197dd8f94047b359ca0a78695144304cb45f87c9eb'),
    'packaging-26.3': ('https://files.pythonhosted.org/packages/7d/fa/3944b40b07da9ce895c0e6303a5ab7d53da063554f534556b134a54d6093/packaging-26.3.tar.gz',
                       '94edc256424af38762eb31306eed28beb9f0efc50a8837492c9d6fd6004aed79'),
    'requests-2.34.2': ('https://files.pythonhosted.org/packages/ac/c3/e2a2b89f2d3e2179abd6d00ebd70bff6273f37fb3e0cc209f48b39d00cbf/requests-2.34.2.tar.gz',
                        'f288924cae4e29463698d6d60bc6a4da69c89185ad1e0bcc4104f584e960b9ed'),
}

MODELS = {
    # repo: commit; only these files are fetched
    'hf-internal-testing/tiny-random-gpt2': '71034c5d8bde858ff824298bdedc65515b97d2b9',
    'distilbert/distilgpt2': '2290a62682d06624634c1f46a6ad5be0f47f38aa',
}
MODEL_FILES = ['*.json', '*.safetensors', '*.txt']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def inputs(rw):
    dl = os.path.join(rw, 'downloads')
    os.makedirs(dl, exist_ok=True)
    for name, (url, want) in INPUTS.items():
        dest = os.path.join(rw, 'inputs', name)
        if os.path.isdir(dest):
            continue
        arc = os.path.join(dl, url.rsplit('/', 1)[1])
        if not os.path.exists(arc) or sha256(arc) != want:
            print('fetch', url)
            urllib.request.urlretrieve(url, arc)
        got = sha256(arc)
        if got != want:
            sys.exit('%s: sha256 %s, expected %s' % (arc, got, want))
        with tarfile.open(arc) as t:
            t.extractall(os.path.join(rw, 'inputs'), filter='data')


def models(rw):
    os.environ['HF_HOME'] = os.path.join(rw, 'hf')
    from huggingface_hub import snapshot_download
    for repo, rev in MODELS.items():
        print(snapshot_download(repo, revision=rev, allow_patterns=MODEL_FILES))


if __name__ == '__main__':
    {'inputs': inputs, 'models': models}[sys.argv[2]](sys.argv[1])
