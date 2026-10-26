#!/usr/bin/env python3
"""Pin research-route checks to the actual function, preserving rejection cases."""
import unittest

import validate_kernel_knowledge as validator


class KernelKnowledgeRouteTests(unittest.TestCase):
    @staticmethod
    def route_body(routes=None):
        if routes is None:
            routes = sorted(validator.EXPECTED_ROUTES - {""})
        return "\n".join(
            f'if (routeId == QStringLiteral("{route}")) {{ switchPage(); }}'
            for route in routes
        )

    @classmethod
    def definition(cls, routes=None):
        return ("void KernelDock::openKnowledgeRoute(const QString& routeId)\n{\n"
                + cls.route_body(routes) + "\n}\n")

    def test_bool_neighbor_does_not_hide_existing_route_function(self):
        source = self.definition() + "bool KernelDock::ensureTabInitialized(int) { return true; }"
        self.assertEqual([], validator.validate_kernel_routes(source))

    def test_neighbor_can_precede_function_or_be_absent(self):
        source = "bool KernelDock::ensureTabInitialized(int) { return true; }\n"
        self.assertEqual([], validator.validate_kernel_routes(source + self.definition()))
        self.assertEqual([], validator.validate_kernel_routes(self.definition()))

    def test_multiline_signature_and_nested_bodies(self):
        source = ("void\nKernelDock :: openKnowledgeRoute\n(const QString& routeId)\n{\n"
                  "if (ready()) { auto callback = [] { return '}'; }; callback(); }\n"
                  + self.route_body() + "\n}\n")
        self.assertEqual([], validator.validate_kernel_routes(source))

    def test_braces_in_comments_and_literals_do_not_change_boundary(self):
        body = ("// } } }\n/* { } */\n"
                'auto message = QStringLiteral("escaped quote: \\\" } {");\n'
                'auto raw = R"marker( } " { )marker";\n'
                + self.route_body())
        source = "void KernelDock::openKnowledgeRoute(const QString& routeId) {\n" + body + "\n}\n"
        self.assertEqual([], validator.validate_kernel_routes(source))

    def test_outside_route_and_commented_route_are_excluded(self):
        source = ("// void KernelDock::openKnowledgeRoute() { broken(); }\n"
                  + self.definition() + "void KernelDock::other() {\n"
                  + self.route_body(["unknown"]) + "\n}\n")
        self.assertEqual([], validator.validate_kernel_routes(source))

    def test_missing_declaration_only_and_unterminated_are_rejected(self):
        sources = (
            "void KernelDock::other() {}",
            "void KernelDock::openKnowledgeRoute(const QString& routeId);\n"
            + "void KernelDock::other() {\n" + self.route_body() + "\n}",
            "void KernelDock::openKnowledgeRoute(const QString& routeId) {\n"
            + self.route_body(),
        )
        for source in sources:
            with self.subTest(source=source[:80]):
                self.assertEqual(["KernelDock::openKnowledgeRoute function is missing"],
                                 validator.validate_kernel_routes(source))

    def test_missing_route_is_still_rejected(self):
        routes = sorted(validator.EXPECTED_ROUTES - {"", "cid"})
        self.assertEqual(["KernelDock routes missing: cid"],
                         validator.validate_kernel_routes(self.definition(routes)))

    def test_extra_route_is_still_rejected(self):
        routes = sorted(validator.EXPECTED_ROUTES - {""}) + ["unknown"]
        self.assertEqual(["KernelDock routes not whitelisted: unknown"],
                         validator.validate_kernel_routes(self.definition(routes)))

    def test_duplicate_route_is_still_rejected(self):
        routes = sorted(validator.EXPECTED_ROUTES - {""}) + ["cid"]
        self.assertEqual(["duplicate KernelDock routes: cid"],
                         validator.validate_kernel_routes(self.definition(routes)))

    def test_current_production_routes(self):
        source = validator.KERNEL_DOCK_PATH.read_text(encoding="utf-8")
        self.assertEqual([], validator.validate_kernel_routes(source))


if __name__ == "__main__":
    unittest.main()
