"""Start-up dominated by imports: a tool that imports much of the standard library and does a little with it."""
import argparse
import collections
import configparser
import csv
import dataclasses
import datetime
import decimal
import difflib
import email.message
import fractions
import functools
import html.parser
import http.client
import inspect
import io
import itertools
import json
import logging
import pathlib
import pprint
import re
import shlex
import statistics
import string
import textwrap
import typing
import unittest
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile
import zlib

CONFIG = """[server]
host = example.org
port = 8080
[paths]
root = /srv/data
logs = /var/log/app
"""

PAGE = "<html><body><h1>Title</h1><p class='x'>One <a href='/a'>two</a></p></body></html>"


@dataclasses.dataclass
class Job:
    name: str
    size: int
    tags: typing.List[str] = dataclasses.field(default_factory=list)


class LinkParser(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []

    def handle_starttag(self, tag, attrs):
        if tag == 'a':
            self.links.extend(v for k, v in attrs if k == 'href')


def work(i):
    parser = argparse.ArgumentParser(prog='tool')
    parser.add_argument('--count', type=int, default=1)
    parser.add_argument('files', nargs='*')
    args = parser.parse_args(shlex.split('--count %d a.txt b.txt' % i))
    cfg = configparser.ConfigParser()
    cfg.read_string(CONFIG)
    rows = list(csv.reader(io.StringIO('a,b,c\n1,2,3\n%d,5,6\n' % i)))
    jobs = [Job('job%d' % k, k * i, ['x'] * k) for k in range(3)]
    lp = LinkParser()
    lp.feed(PAGE)
    root = ET.Element('jobs')
    for j in jobs:
        ET.SubElement(root, 'job', name=j.name, size=str(j.size))
    msg = email.message.EmailMessage()
    msg['Subject'] = 'Report %d' % i
    msg.set_content(textwrap.fill('word ' * 30, 40))
    log = logging.getLogger('tool')
    parts = [
        repr(args.count), repr(args.files), cfg['server']['port'],
        pathlib.PurePosixPath(cfg['paths']['root'], 'f%d' % i).as_posix(),
        repr(rows), json.dumps([dataclasses.asdict(j) for j in jobs]),
        repr(lp.links), ET.tostring(root).decode(), msg.as_string(),
        urllib.parse.urlencode({'q': 'a b', 'n': i}),
        str(statistics.mean([1, 2, 3, i])), str(fractions.Fraction(i, 6)),
        str(decimal.Decimal(i) / 7), pprint.pformat(collections.Counter('abracadabra')),
        str(round(difflib.SequenceMatcher(None, 'kitten', 'sitting').ratio(), 4)),
        string.capwords('hello world %d' % i), re.sub(r'\d', '#', 'a1b22'),
        str(datetime.date(2024, 1, 1) + datetime.timedelta(days=i)),
        str(len(inspect.signature(work).parameters)),
        str(functools.reduce(lambda a, b: a * b, range(1, 6))),
        str(list(itertools.islice(itertools.count(i), 3))),
        log.name, http.client.responses[200], unittest.TestCase.__name__,
        zipfile.ZIP_DEFLATED.__class__.__name__,
    ]
    return zlib.crc32('\n'.join(parts).encode())


def main(n):
    return [work(i) for i in range(n)]


N = 1

if __name__ == '__main__':
    print(main(N))
