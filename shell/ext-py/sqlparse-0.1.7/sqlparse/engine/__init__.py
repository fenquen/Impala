# Copyright (C) 2008 Andi Albrecht, albrecht.andi@gmail.com
#
# This module is part of python-sqlparse and is released under
# the BSD License: http://www.opensource.org/licenses/bsd-license.php.

"""filter"""

from sqlparse import lexer
from sqlparse.engine import grouping
from sqlparse.engine.filter import StatementFilter

# XXX remove this when cleanup is complete
Filter = object


class FilterStack(object):

    def __init__(self):
        self.preprocess = []
        self.stmtprocess = []
        self.postprocess = []
        self.split_statements = False
        self._grouping = False

    def _flatten(self, stream):
        for token in stream:
            if token.is_group():
                for t in self._flatten(token.tokens):
                    yield t
            else:
                yield token

    def enable_grouping(self):
        self._grouping = True

    def full_analyze(self):
        self.enable_grouping()

    def run(self, sql, encoding=None):
        stream = lexer.tokenize(sql, encoding)
        # Process token stream
        if self.preprocess:
            for filter_ in self.preprocess:
                stream = filter_.process(self, stream)

        if (self.stmtprocess or self.postprocess or self.split_statements
            or self._grouping):
            splitter = StatementFilter()
            stream = splitter.process(self, stream)

        # import StripCommentsFilter in the run() method to avoid a circular dependency.
        # For stripping comments, the only grouping method we want to invoke is
        # grouping.group(), this considerably improves performance.
        strip_comments_only = False
        if self.stmtprocess and len(self.stmtprocess) == 1:
          from sqlparse.filters import StripCommentsFilter
          strip_comments_only = isinstance(self.stmtprocess[0], StripCommentsFilter)

        if self._grouping:
            def _group(stream):
                for stmt in stream:
                    if strip_comments_only:
                        grouping.group_comments(stmt)
                    else:
                        grouping.group(stmt)
                    yield stmt
            stream = _group(stream)

        if self.stmtprocess:
            def _run1(stream):
                ret = []
                for stmt in stream:
                    for filter_ in self.stmtprocess:
                        filter_.process(self, stmt)
                    ret.append(stmt)
                return ret
            stream = _run1(stream)

        if self.postprocess:

            def _run2(stream):
                for stmt in stream:
                    stmt.tokens = list(self._flatten(stmt.tokens))
                    for filter_ in self.postprocess:
                        stmt = filter_.process(self, stmt)
                    yield stmt
            stream = _run2(stream)

        return stream
