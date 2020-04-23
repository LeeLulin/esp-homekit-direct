#!/usr/bin/env python

from collections import namedtuple
from flask import Flask, render_template, render_template_string, request
import os
import os.path


Network = namedtuple('Network', ['ssid', 'secure'])

WIFI_NETWORKS = [
    Network(ssid='Network1', secure=False),
    Network(ssid='Network2', secure=False),
    Network(ssid='Network3', secure=False),
    Network(ssid='Network4', secure=True),
    Network(ssid='Network5', secure=True),
    Network(ssid='Network6', secure=True),
    Network(ssid='MaximumNetworkNameMaximumNetwork', secure=False),
    Network(ssid='MaximumNetworkNameMaximumNetwork', secure=True),
]


base_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), '..')
app = Flask(
    __name__,
    static_folder=os.path.join(base_path, 'content'),
    template_folder=os.path.join(base_path, 'content'),
)


def get_custom_html():
    if 'WIFI_CONFIG_CUSTOM_HTML' not in os.environ:
        return ''
    with open(os.environ['WIFI_CONFIG_CUSTOM_HTML']) as f:
        return f.read()


@app.template_filter('ternary')
def ternary(x, true, false):
    return true if x else false


def _render_settings(networks):
    if 'WIFI_CONFIG_INDEX_HTML' in os.environ:
        with open(os.environ['WIFI_CONFIG_INDEX_HTML']) as f:
            template = f.read()

        return render_template_string(
            template,
            networks=networks,
            custom_html=get_custom_html(),
        )

    return render_template(
        'index.html',
        networks=WIFI_NETWORKS,
        custom_html=get_custom_html(),
    )



@app.route('/settings', methods=['GET'])
def get_settings():
    return _render_settings(WIFI_NETWORKS)


@app.route('/settings0', methods=['GET'])
def get_settings0():
    return _render_settings([])


@app.route('/settings', methods=['POST'])
def update_settings():
    if request.form.get('password'):
        return 'Connecting to "%s", password = "%s"' % (
            request.form['ssid'],
            request.form['password']
        )

    return 'Connecting to "%s", no password' % request.form['ssid']


app.run(host='0.0.0.0', debug=True)
