"""Coroutines and async/await: an asyncio crawler with worker tasks sharing a queue."""
import asyncio


async def fetch(url, depth):
    await asyncio.sleep(0)
    h = 0
    for ch in url:
        h = (h * 131 + ord(ch)) & 0xffffff
    if depth >= 3:
        return h, []
    return h, ['%s/%d' % (url, (h >> k) % 10) for k in (0, 4, 8)]


async def worker(queue, seen, results):
    while True:
        url, depth = await queue.get()
        h, links = await fetch(url, depth)
        results.append(h)
        for link in links:
            if link not in seen:
                seen.add(link)
                await queue.put((link, depth + 1))
        queue.task_done()


async def crawl(roots, workers):
    queue = asyncio.Queue()
    seen = set(roots)
    results = []
    for r in roots:
        queue.put_nowait((r, 0))
    tasks = [asyncio.ensure_future(worker(queue, seen, results)) for _ in range(workers)]
    await queue.join()
    for t in tasks:
        t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    return results


def main(n):
    roots = ['https://site%d.example' % i for i in range(n)]
    loop = asyncio.new_event_loop()
    try:
        results = loop.run_until_complete(crawl(roots, 8))
    finally:
        loop.close()
    return len(results), sum(results) % 1000000007, results[:5]


N = 60

if __name__ == '__main__':
    print(main(N))
