"""transformers: greedy generate(), batch 1, 64 new tokens, on CPU with one thread.
argv: model repo, revision, number of generations. Prints import, load and generation
times apart (on stderr), and the generated token ids' digest."""
import time
t_import = time.perf_counter()
import hashlib, sys
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

name, rev, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
torch.set_num_threads(1)
torch.manual_seed(0)
t0 = time.perf_counter()
tok = AutoTokenizer.from_pretrained(name, revision=rev)
model = AutoModelForCausalLM.from_pretrained(name, revision=rev).eval()
ids = tok('The quick brown fox jumps over the lazy dog because', return_tensors='pt')
t1 = time.perf_counter()
h = hashlib.sha256()
with torch.inference_mode():
    for i in range(n):
        out = model.generate(**ids, max_new_tokens=64, min_new_tokens=64, do_sample=False,
                             pad_token_id=tok.eos_token_id)
        h.update(str(out[0].tolist()).encode())
t2 = time.perf_counter()
print('import %.2f s, load %.2f s, generate %.2f s, %.2f ms/token' % (t0 - t_import, t1 - t0, t2 - t1, 1000 * (t2 - t1) / (n * 64)), file=sys.stderr)
print('tokens', h.hexdigest())
