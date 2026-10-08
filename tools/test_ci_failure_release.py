"""Offline regression tests; all GitHub calls and release mutations are mocked."""
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

import ci_failure_release as release


SHA = "a" * 40
MODULE = "KswordARKLight-unsigned-Release"


def archive_bytes():
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr("Release/KswordARKLight.exe", b"fixture")
    return output.getvalue()


class FakeGh:
    def __init__(self):
        self.calls = []
        self.downloads = []
        self.driver_status = "completed"
        self.driver_conclusion = "success"
        self.driver_sha = SHA
        self.job_failure = True
        self.job_api_error = False
        self.driver_api_error = False
        self.artifact_error = False
        self.corrupt_artifact = False
        self.artifact_sha = SHA
        self.artifact_run = 10
        self.releases = []

    def json(self, endpoint):
        if "/workflows/driver-ci.yml/runs" in endpoint:
            if self.driver_api_error:
                raise RuntimeError("Driver status unavailable")
            return {"workflow_runs": [{"id": 20, "head_sha": SHA}]}
        if endpoint.endswith("/actions/runs/20"):
            return {"head_sha": self.driver_sha, "head_branch": "main", "event": "push",
                    "status": self.driver_status, "conclusion": self.driver_conclusion}
        if "/commits/" in endpoint:
            return {"commit": {"message": "fixture title\nsecond line"}}
        raise AssertionError(endpoint)

    def pages(self, endpoint, key=None):
        if key == "jobs":
            if self.job_api_error:
                raise RuntimeError("Jobs unavailable")
            if "/runs/20/" in endpoint:
                return [{"id": 200, "name": "Driver Release build", "status": self.driver_status,
                         "conclusion": self.driver_conclusion, "html_url": "https://github.com/driver", "steps": []}]
            assert "/attempts/2/jobs" in endpoint
            return [{"id": 100, "name": "User-mode Release build", "status": "completed",
                     "conclusion": "failure" if self.job_failure else "success", "html_url": "https://github.com/job",
                     "steps": [{"name": "Compile C++", "conclusion": "failure" if self.job_failure else "success"}]},
                    {"id": 101, "name": "KswordSetup Release build", "conclusion": "skipped", "steps": []},
                    {"id": 102, "name": "Publish automatic prerelease", "conclusion": None, "status": "in_progress"}]
        if key == "artifacts":
            if "/runs/20/" in endpoint:
                return []
            return [{"id": 300, "name": MODULE, "expired": False,
                     "workflow_run": {"head_sha": self.artifact_sha, "id": self.artifact_run, "head_branch": "main"}},
                    {"id": 301, "name": "ioctl-audit", "expired": False},
                    {"id": 302, "name": "KswordSetup-unsigned-Release", "expired": True}]
        if "/releases?" in endpoint:
            return self.releases
        raise AssertionError(endpoint)

    def download(self, endpoint):
        self.downloads.append(endpoint)
        if endpoint.endswith("/logs"):
            return b"noise\n\x1b[31mfile.cpp(4): error C4996: deprecated Qt overload\x1b[0m\n##[error]Build failed\n"
        if self.artifact_error:
            raise RuntimeError("Artifact download unavailable")
        return b"not a ZIP" if self.corrupt_artifact else archive_bytes()

    def call(self, *args):
        self.calls.append(args)
        return b"https://github.com/test/repo/releases/tag/fixture\n"


class FailureReleaseTests(unittest.TestCase):
    def publish(self, gh, root, needs=None, **kwargs):
        return release.publish(gh, "test/repo", SHA, "10", "2", root,
                               needs if needs is not None else {"usermode-release": {"result": "failure"}}, **kwargs)

    def test_failed_build_publishes_errors_and_available_current_modules(self):
        gh = FakeGh()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            url = self.publish(gh, root)
            self.assertIn("github.com", url)
            notes = (root / "CI_ERRORS.md").read_text(encoding="utf-8")
            for expected in ("CI FAILED", "error C4996", "Compile C++", "skipped", MODULE, "未构建", "完整构建未通过"):
                self.assertIn(expected, notes)
            self.assertNotIn("\x1b", notes)
            self.assertNotIn("Publish automatic prerelease", notes)
            with zipfile.ZipFile(root / f"KswordCI-Diagnostics-{SHA[:8]}.zip") as archive:
                self.assertIn("job-100.log", archive.namelist())
                self.assertNotIn("job-101.log", archive.namelist())
                status = json.loads(archive.read("CI_STATUS.json"))
                self.assertEqual(status["artifacts"][0]["sha"], SHA)
            create = next(c for c in gh.calls if c[:2] == ("release", "create"))
            self.assertIn(str(root / f"{MODULE}.zip"), create)
            self.assertIn("--latest=false", create)
            self.assertIn("[FAILED]", create[create.index("--title") + 1])

    def test_driver_failure_alone_generates_failed_release(self):
        gh = FakeGh()
        gh.job_failure = False
        gh.driver_conclusion = "failure"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.publish(gh, root, {"usermode-release": {"result": "success"}}, driver_id="20")
            notes = (root / "CI_ERRORS.md").read_text(encoding="utf-8")
            self.assertIn("Driver CI：completed / failure", notes)
            self.assertIn("Driver Release build 错误摘要", notes)

    def test_driver_missing_and_timeout_do_not_block_diagnostic_release(self):
        for kind in ("missing", "timeout", "wrong-sha"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                gh = FakeGh()
                gh.driver_api_error = kind == "missing"
                gh.driver_status = "in_progress" if kind == "timeout" else "completed"
                gh.driver_sha = "b" * 40 if kind == "wrong-sha" else SHA
                root = Path(directory)
                self.publish(gh, root)
                self.assertIn("Driver CI", (root / "CI_ERRORS.md").read_text(encoding="utf-8"))
                self.assertTrue(any(c[:2] == ("release", "create") for c in gh.calls))

    def test_unavailable_job_logs_still_preserve_failure_status(self):
        gh = FakeGh()
        gh.job_api_error = True
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.publish(gh, root)
            notes = (root / "CI_ERRORS.md").read_text(encoding="utf-8")
            self.assertIn("状态查询失败", notes)
            self.assertIn("usermode-release | failure", notes)

    def test_corrupt_or_unavailable_downloads_still_publish_diagnostics(self):
        for kind in ("corrupt", "unavailable"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                gh = FakeGh()
                gh.corrupt_artifact = kind == "corrupt"
                gh.artifact_error = kind == "unavailable"
                root = Path(directory)
                self.publish(gh, root)
                self.assertFalse((root / f"{MODULE}.zip").exists())
                self.assertIn("下载失败", (root / "CI_ERRORS.md").read_text(encoding="utf-8"))
                self.assertTrue(any(c[:2] == ("release", "create") for c in gh.calls))

    def test_foreign_artifact_is_never_downloaded_or_published(self):
        for kind in ("sha", "run"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                gh = FakeGh()
                gh.artifact_sha = "b" * 40 if kind == "sha" else SHA
                gh.artifact_run = 99 if kind == "run" else 10
                root = Path(directory)
                self.publish(gh, root)
                self.assertFalse((root / f"{MODULE}.zip").exists())
                self.assertFalse(any("artifacts/300/zip" in d for d in gh.downloads))

    def test_packaging_failure_is_in_release_body_and_diagnostic_archive(self):
        gh = FakeGh()
        gh.job_failure = False
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "publish.log"
            log.write_text("Required automatic release artifact is unavailable: Setup\n", encoding="utf-8")
            self.publish(gh, root, {}, publication_log=log)
            self.assertIn("Required automatic release artifact", (root / "CI_ERRORS.md").read_text(encoding="utf-8"))
            with zipfile.ZipFile(root / f"KswordCI-Diagnostics-{SHA[:8]}.zip") as archive:
                self.assertIn("release-publication.log", archive.namelist())

    def test_retention_and_retry_never_mutate_manual_releases(self):
        gh = FakeGh()
        tag = f"ci-build-{SHA[:8]}-10-2"
        def automatic(index):
            return {"tag_name": tag if index == 5 else f"ci-build-old-{index}", "name": "[CI Build] fixture",
                    "created_at": f"2026-10-0{index}", "prerelease": True, "draft": False,
                    "target_commitish": SHA, "html_url": "https://github.com/existing"}
        gh.releases = [automatic(i) for i in range(1, 6)] + [
            {"tag_name": "manual", "name": "Manual", "created_at": "2026-10-09", "prerelease": True},
            {"tag_name": "ci-build-manual", "name": "Manual", "prerelease": True},
            {"tag_name": "regular", "name": "[CI Build] regular", "prerelease": False}]
        with tempfile.TemporaryDirectory() as directory:
            self.publish(gh, Path(directory))
        self.assertTrue(any(c[:2] == ("release", "edit") for c in gh.calls))
        self.assertTrue(any(c[:2] == ("release", "upload") for c in gh.calls))
        self.assertFalse(any(c[:2] == ("release", "create") for c in gh.calls))
        deleted = [c[2] for c in gh.calls if c[:2] == ("release", "delete")]
        self.assertEqual(deleted, ["ci-build-old-2", "ci-build-old-1"])

    def test_unrelated_existing_release_blocks_update(self):
        gh = FakeGh()
        gh.releases = [{"tag_name": f"ci-build-{SHA[:8]}-10-2", "name": "Manual", "prerelease": True}]
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(RuntimeError):
            self.publish(gh, Path(directory))
        self.assertFalse(gh.calls)

    def test_error_excerpt_bounds_and_escapes_fences(self):
        text = "\n".join("error C4996: ```" + "a" * 2000 for _ in range(30))
        excerpt = release.error_excerpt(text)
        self.assertLessEqual(len(excerpt), 6000)
        self.assertNotIn("```", excerpt)

    def test_workflow_routes_failures_without_weakening_build_gates(self):
        root = Path(__file__).resolve().parent.parent
        workflow = (root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        publication = workflow.split("  auto-prerelease:", 1)[1]
        condition = publication.split("    if: >-", 1)[1].split("    runs-on:", 1)[0]
        self.assertIn("always()", condition)
        self.assertIn("github.event_name == 'push'", condition)
        self.assertIn("github.ref == 'refs/heads/main'", condition)
        self.assertNotIn("needs.", condition)
        self.assertIn("CI_JOB_RESULTS: ${{ toJSON(needs) }}", publication)
        self.assertIn("publish_status=${PIPESTATUS[0]}", publication)
        self.assertIn("--publication-error-log", publication)
        self.assertNotIn("continue-on-error:", workflow)
        self.assertIn("needs.source-integrity.result == 'success'", workflow)


if __name__ == "__main__":
    unittest.main()
