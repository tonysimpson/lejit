"""PyYAML: safe_load_all / safe_dump_all of Kubernetes-style manifests (the default, pure-Python loader)."""
import hashlib
import yaml


def manifest(n):
    return {'apiVersion': 'apps/v1', 'kind': 'Deployment',
            'metadata': {'name': 'web%d' % n, 'labels': {'app': 'web', 'tier': 'fe'}},
            'spec': {'replicas': 3, 'selector': {'matchLabels': {'app': 'web'}},
                     'template': {'spec': {'containers': [
                         {'name': 'c%d' % i, 'image': 'nginx:1.%d' % i, 'ports': [{'containerPort': 80 + i}],
                          'resources': {'limits': {'cpu': '500m', 'memory': '128Mi'}},
                          'env': [{'name': 'K%d' % j, 'value': 'v%d' % j} for j in range(10)]} for i in range(5)]}}}}


text = yaml.safe_dump_all([manifest(n) for n in range(200)])
for i in range(3):
    docs = list(yaml.safe_load_all(text))
    out = yaml.safe_dump_all(docs)
assert out == text
print('documents', len(docs))
print('yaml', hashlib.sha256(out.encode()).hexdigest())
