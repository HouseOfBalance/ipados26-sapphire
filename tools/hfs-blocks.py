#!/usr/bin/env python3
"""Alias wrapper around hfs-push-ranges.py so both tools share identical journal/extent/diff logic."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module
import_module('hfs-push-ranges').compute_runs(sys.argv)
