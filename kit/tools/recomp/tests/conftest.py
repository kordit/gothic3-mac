# test_translate.py is the differential test against Unicorn, a script with its own main() and lock
# (`python tools/recomp/tests/test_translate.py`), not a pytest module: collected, its functions
# asked for fixtures that do not exist (emu, when) or ran before main() set the paths.
collect_ignore = ["test_translate.py"]
