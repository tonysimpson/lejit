"""boto3/botocore: S3 and DynamoDB calls with real validation, serialising, SigV4 signing
and parsing; only the HTTP send is replaced, by recorded-shape responses."""
import hashlib, json
import boto3
from botocore.awsrequest import AWSResponse


class Raw:
    def __init__(self, body):
        self.body = body

    def stream(self, **kw):
        yield self.body


S3 = ('<?xml version="1.0" encoding="UTF-8"?><ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'
      '<Name>b</Name><Prefix>dir/</Prefix><KeyCount>200</KeyCount><MaxKeys>1000</MaxKeys><IsTruncated>false</IsTruncated>' +
      ''.join('<Contents><Key>dir/file%d.txt</Key><LastModified>2024-01-01T00:00:00.000Z</LastModified><ETag>"abc%d"</ETag>'
              '<Size>%d</Size><StorageClass>STANDARD</StorageClass></Contents>' % (i, i, i * 10) for i in range(200)) +
      '</ListBucketResult>').encode()
DDB = json.dumps({'Count': 50, 'ScannedCount': 50, 'Items': [
    {'pk': {'S': 'k%d' % i}, 'n': {'N': str(i)}, 'm': {'M': {'a': {'S': 'x'}, 'b': {'L': [{'N': '1'}, {'N': '2'}]}}}}
    for i in range(50)]}).encode()


def fake_send(request, **kw):
    body = S3 if 's3.' in request.url else (DDB if b'"Item"' not in (request.body or b'') else b'{}')
    return AWSResponse(request.url, 200, {'x-amzn-requestid': 'r'}, Raw(body))


h = hashlib.sha256()
sess = boto3.session.Session(aws_access_key_id='AK', aws_secret_access_key='SK', region_name='us-east-1')
for k in range(5):
    s3 = sess.client('s3')
    ddb = sess.client('dynamodb')
    for c in (s3, ddb):
        c.meta.events.register('before-send', fake_send)
    for i in range(300):
        r = s3.list_objects_v2(Bucket='b', Prefix='dir/')
        q = ddb.query(TableName='t', KeyConditionExpression='pk = :v',
                      ExpressionAttributeValues={':v': {'S': 'k%d' % i}})
        p = ddb.put_item(TableName='t', Item={'pk': {'S': 'k%d' % i}, 'v': {'N': str(i)}})
        for x in (r, q, p):
            x.pop('ResponseMetadata', None)
            h.update(json.dumps(x, default=str, sort_keys=True).encode())
print('keys', r['KeyCount'], 'items', q['Count'])
print('responses', h.hexdigest())
