"""How a translation stops short of a construct."""


class Refuse(Exception):
    """The function can't be generated: no rule for a construct."""


class ResumeHere(Exception):
    """No compile-time form for this construct: CPython's code runs from
    the statement being translated (resume.resume decides whether it may)."""
