"""What lejit.runtime_features() finds on the running interpreter, printed as
JSON: the two detectors the build runs (installed.py and strings.py) and
the layout probe of the compiled module (lejit._core._layouts). lejit runs it in
a subprocess, with -I, so that a probe that crashes can't harm the
program.

setup.py copies this file and the detector files it needs into lejit/_features,
next to the compiled module.

    python -I check.py [DIR]
        print this interpreter's answers as JSON, with the struct layouts
        when DIR, the directory that holds the lejit package, is given
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def detect(le_parent=None):
    """{'features': {NI_HAS_name: 0 or 1}, 'messages': {NI_MSG_name: text},
    'layouts': {struct: size or what is wrong}} for this interpreter, or
    {'error': why}. A feature the detector can't answer is left out, so
    that it differs from the build."""
    sys.path.insert(0, HERE)
    import features
    import installed
    import strings
    try:
        probed, found, _ = installed.detect(sys.executable)
        messages = strings.probe(sys.executable)
    except (features.FeatureError, strings.StringError) as e:
        return {'error': str(e)}
    answers = {'features': {f[0]: f[1] for f in found},
               'messages': {'NI_MSG_' + k: v for k, v in messages.items()
                            if v is not None}}
    # some features are answered only by the headers; without them those
    # are missing here, and the message says why
    if not os.path.isfile(os.path.join(probed['include'], 'Python.h')):
        answers['no_headers'] = probed['include']
    if le_parent is not None:
        sys.path.insert(0, le_parent)
        from lejit import _core
        answers['layouts'] = _core._layouts()
    return answers


if __name__ == '__main__':
    json.dump(detect(sys.argv[1] if len(sys.argv) > 1 else None), sys.stdout)
