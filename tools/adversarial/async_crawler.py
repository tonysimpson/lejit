"""Coroutines and async/await: an asyncio crawler with worker tasks sharing a queue. Variant 1 crawls other sites, with fewer workers and a wider, shallower link tree."""
import asyncio


async def fetch(url, depth, max_depth, shifts):
    await asyncio.sleep(0)
    h = 0
    for ch in url:
        h = (h * 131 + ord(ch)) & 0xffffff
    if depth >= max_depth:
        return h, []
    return h, ['%s/%d' % (url, (h >> k) % 10) for k in shifts]


async def worker(queue, seen, results, max_depth, shifts):
    while True:
        url, depth = await queue.get()
        h, links = await fetch(url, depth, max_depth, shifts)
        results.append(h)
        for link in links:
            if link not in seen:
                seen.add(link)
                await queue.put((link, depth + 1))
        queue.task_done()


async def crawl(roots, workers, max_depth, shifts):
    queue = asyncio.Queue()
    seen = set(roots)
    results = []
    for r in roots:
        queue.put_nowait((r, 0))
    tasks = [asyncio.ensure_future(worker(queue, seen, results, max_depth, shifts)) for _ in range(workers)]
    await queue.join()
    for t in tasks:
        t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    return results


SHAPES = [('https://site%d.example', 8, 3, (0, 4, 8)),
          ('http://shop%d.example.net/en', 5, 2, (0, 3, 6, 9, 12))]


def main(n, variant=0):
    pattern, workers, max_depth, shifts = SHAPES[variant]
    roots = [pattern % i for i in range(n)]
    loop = asyncio.new_event_loop()
    try:
        results = loop.run_until_complete(crawl(roots, workers, max_depth, shifts))
    finally:
        loop.close()
    return len(results), sum(results) % 1000000007, results[:5]


N = 60

if __name__ == '__main__':
    print(main(N))
