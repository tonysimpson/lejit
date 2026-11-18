"""Django app for gunicorn: ORM query plus template, default middleware, sqlite in memory."""
import datetime
import django
from django.conf import settings

TEMPLATE = '''<html><body><h1>{{ title }}</h1>
<table>{% for u in users %}<tr class="{% cycle 'a' 'b' %}"><td>{{ u.username|title }}</td><td>{{ u.email|lower }}</td>
<td>{{ u.date_joined|date:"Y-m-d" }}</td><td>{% if u.is_staff %}staff{% else %}user{% endif %}</td></tr>{% endfor %}</table>
<p>{{ users|length }} users</p></body></html>'''

settings.configure(
    DEBUG=False, SECRET_KEY='x', ALLOWED_HOSTS=['*'], ROOT_URLCONF=__name__, USE_TZ=True, TIME_ZONE='UTC',
    DATABASES={'default': {'ENGINE': 'django.db.backends.sqlite3', 'NAME': ':memory:'}},
    INSTALLED_APPS=['django.contrib.contenttypes', 'django.contrib.auth', 'django.contrib.sessions', 'django.contrib.messages'],
    MIDDLEWARE=['django.middleware.security.SecurityMiddleware', 'django.contrib.sessions.middleware.SessionMiddleware',
                'django.middleware.common.CommonMiddleware', 'django.middleware.csrf.CsrfViewMiddleware',
                'django.contrib.auth.middleware.AuthenticationMiddleware', 'django.contrib.messages.middleware.MessageMiddleware'],
    TEMPLATES=[{'BACKEND': 'django.template.backends.django.DjangoTemplates', 'DIRS': [], 'APP_DIRS': False,
                'OPTIONS': {'loaders': [('django.template.loaders.locmem.Loader', {'list.html': TEMPLATE})]}}])
django.setup()

from django.contrib.auth.models import User
from django.core.management import call_command
from django.core.wsgi import get_wsgi_application
from django.http import JsonResponse
from django.shortcuts import get_object_or_404, render
from django.urls import path


def users(request):
    qs = User.objects.filter(is_active=True).order_by('username')[:50]
    return render(request, 'list.html', {'title': 'Users', 'users': qs})


def user(request, n):
    u = get_object_or_404(User, username='user%d' % n)
    return JsonResponse({'username': u.username, 'email': u.email, 'staff': u.is_staff,
                         'joined': u.date_joined.isoformat()})


urlpatterns = [path('users/', users), path('user/<int:n>/', user)]

call_command('migrate', verbosity=0)
JOINED = datetime.datetime(2024, 1, 1, tzinfo=datetime.timezone.utc)
User.objects.bulk_create([User(username='user%d' % i, email='U%d@Example.com' % i, is_staff=i % 7 == 0,
                               date_joined=JOINED) for i in range(200)])
application = get_wsgi_application()
