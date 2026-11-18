"""Flask app for gunicorn: a Jinja2 page with loops, filters and url_for, and a JSON route."""
from flask import Flask, jsonify, render_template_string

app = Flask(__name__)
PAGE = '''<html><body><h1>{{ title }}</h1><ul>{% for p in posts %}<li class="{{ loop.cycle('a','b') }}"><a href="{{ url_for('post', pid=p.id) }}">{{ p.title|title }}</a>
<span>{{ p.body|truncate(40) }}</span>{% if p.tags %}{% for t in p.tags %}<i>{{ t }}</i>{% endfor %}{% endif %}</li>{% endfor %}</ul></body></html>'''
POSTS = [dict(id=i, title='post number %d' % i, body='lorem ipsum dolor sit amet ' * 5, tags=['a', 'b'][:i % 3])
         for i in range(30)]


@app.route('/')
def index():
    return render_template_string(PAGE, title='Blog', posts=POSTS)


@app.route('/post/<int:pid>')
def post(pid):
    return jsonify(POSTS[pid])
