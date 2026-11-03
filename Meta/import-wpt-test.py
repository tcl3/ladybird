#!/usr/bin/env python3

import argparse
import os
import re

from collections import namedtuple
from dataclasses import dataclass
from enum import Enum
from html.parser import HTMLParser
from http.client import InvalidURL
from pathlib import Path
from posixpath import normpath
from urllib.error import HTTPError
from urllib.parse import urljoin
from urllib.parse import urlparse
from urllib.parse import urlsplit
from urllib.parse import urlunsplit
from urllib.request import urlopen

download_exclude_list = {
    # This relies on a dynamic path that cannot be rewitten by the importer.
    "/resources/idlharness.js",
    # This file has been modified to improve the performance of our test runner.
    "/resources/testharness.js",
    # We have modified this file to output text files compatible with our test runner.
    "/resources/testharnessreport.js",
    # We have modified this file to use our internals object instead of relying on WebDriver.
    "/resources/testdriver-vendor.js",
    # Modified to use a relative path, which the importer can't rewrite.
    "/fonts/ahem.css",
}
visited_destinations = set()


class TestType(Enum):
    TEXT = 1, "Tests/LibWeb/Text/input/wpt-import", "Tests/LibWeb/Text/expected/wpt-import"
    REF = 2, "Tests/LibWeb/Ref/input/wpt-import", "Tests/LibWeb/Ref/expected/wpt-import"
    CRASH = 3, "Tests/LibWeb/Crash/wpt-import", ""

    def __new__(cls, *args, **kwds):
        obj = object.__new__(cls)
        obj._value_ = args[0]
        return obj

    def __init__(self, _: int, input_path: str, expected_path: str):
        self.input_path = input_path
        self.expected_path = expected_path


PathMapping = namedtuple("PathMapping", ["source", "destination"])


class ResourceType(Enum):
    INPUT = 1
    EXPECTED = 2


@dataclass
class ResourceAndType:
    resource: str
    type: ResourceType


test_type = TestType.TEXT
raw_reference_paths = []  # As specified in the test HTML
reference_paths = []  # With parent directories


def resource_file_path(resource: str) -> str:
    # A query or fragment (e.g. a form action of /common/blank.html?1) is not part of the file to download.
    return resource.split("#")[0].split("?")[0]


class LinkedResourceFinder(HTMLParser):
    def __init__(self):
        super().__init__()
        self._tag_stack_ = []
        self._match_css_url_ = re.compile(r"url\(['\"]?(?P<url>[^'\")]+)['\"]?\)")
        self._match_css_import_string_ = re.compile(r"@import\s+\"(?P<url>[^\")]+)\"")
        self._match_fetch_import_path = re.compile(r"fetch\((\"|\')(?P<url>.*)(\"|\')\)")
        self._match_import_call = re.compile(r"import\(['\"](?P<url>.*?)['\"].*\)")
        self._match_import_statement = re.compile(r"import (.*? from )?['\"](?P<url>.*?)['\"]")
        self._match_import_scripts = re.compile(r"importScripts\((?P<urls>[^)]*)\)")
        self._match_quoted_string = re.compile(r"['\"](?P<url>[^'\"]*)['\"]")
        self._match_src_assignment = re.compile(r"\.src ?= ?['\"](?P<url>.*?)['\"]")
        self._match_worker_import_path = re.compile(r"Worker\(['\"](?P<url>.*)['\"]\)")
        self._match_service_worker_registration = re.compile(r"serviceWorker\.register\(\s*['\"](?P<url>[^'\"]*)['\"]")
        self._resources = set()
        self._module_resources = set()
        self._worker_resources = set()

    @property
    def resources(self) -> set:
        # An empty path refers to the document itself (e.g. a form action of "" or "?1"), and one ending in a slash to a
        # directory (e.g. a form action of "/"). Neither is a file to import.
        return {
            resource
            for resource in self._resources
            if resource_file_path(resource) and not resource_file_path(resource).endswith("/")
        }

    @property
    def module_resources(self) -> set:
        return self.resources & self._module_resources

    @property
    def worker_resources(self) -> set:
        return self.resources & self._worker_resources

    def handle_starttag(self, tag, attrs):
        self._tag_stack_.append(tag)
        if ":" in tag:
            tag = tag.split(":", 1)[-1]
        if tag in ["script", "img", "iframe", "audio", "video", "source"]:
            attr_dict = dict(attrs)
            if "src" in attr_dict:
                self._resources.add(attr_dict["src"])
            # An SVG script element names its script with href or xlink:href.
            if tag == "script":
                for name in ["href", "xlink:href"]:
                    if name in attr_dict:
                        self._resources.add(attr_dict[name])
        if tag == "link":
            attr_dict = dict(attrs)
            if "rel" in attr_dict and attr_dict["rel"] == "stylesheet":
                self._resources.add(attr_dict["href"])
        if tag == "form":
            attr_dict = dict(attrs)
            if "action" in attr_dict:
                self._resources.add(attr_dict["action"])

    def handle_endtag(self, tag):
        # Close the most recent matching start tag along with any tags left open inside it. End tags without a matching
        # start tag, such as a </html> after an omitted <html>, close nothing.
        if tag in self._tag_stack_:
            while self._tag_stack_.pop() != tag:
                pass

    def handle_data(self, data):
        if self._tag_stack_ and self._tag_stack_[-1] == "style":
            # Look for uses of url()
            url_iterator = self._match_css_url_.finditer(data)
            for match in url_iterator:
                self._resources.add(match.group("url"))
            # Look for @imports that use plain strings - we already found the url() ones
            import_iterator = self._match_css_import_string_.finditer(data)
            for match in import_iterator:
                self._resources.add(match.group("url"))
        elif self._tag_stack_ and self._tag_stack_[-1] == "script":
            self.find_script_resources(data)

    def find_script_resources(self, script):
        # Look for uses of fetch()
        fetch_iterator = self._match_fetch_import_path.finditer(script)
        for match in fetch_iterator:
            self._resources.add(match.group("url"))

        # Look for uses of import()
        import_call_iterator = self._match_import_call.finditer(script)
        for match in import_call_iterator:
            self._resources.add(match.group("url"))
            self._module_resources.add(match.group("url"))

        # Look for uses of import statements
        import_statement_iterator = self._match_import_statement.finditer(script)
        for match in import_statement_iterator:
            self._resources.add(match.group("url"))
            self._module_resources.add(match.group("url"))

        # Look for uses of importScripts()
        import_scripts_iterator = self._match_import_scripts.finditer(script)
        for match in import_scripts_iterator:
            for url_match in self._match_quoted_string.finditer(match.group("urls")):
                self._resources.add(url_match.group("url"))

        # Look for uses of .src = "..."
        src_assignment_iterator = self._match_src_assignment.finditer(script)
        for match in src_assignment_iterator:
            self._resources.add(match.group("url"))

        # Look for uses of Worker()
        filepath_iterator = self._match_worker_import_path.finditer(script)
        for match in filepath_iterator:
            self._resources.add(match.group("url"))
            self._worker_resources.add(match.group("url"))

        # Look for uses of navigator.serviceWorker.register()
        registration_iterator = self._match_service_worker_registration.finditer(script)
        for match in registration_iterator:
            self._resources.add(match.group("url"))
            self._worker_resources.add(match.group("url"))


class TestTypeIdentifier(HTMLParser):
    """Identifies what kind of test the page is, and stores it in self.test_type
    For reference tests, the URLs of the reference pages are saved as self.reference_paths
    """

    def __init__(self, url):
        super().__init__()
        self.url = url
        self.test_type = TestType.TEXT
        self.reference_paths = []

    def handle_starttag(self, tag, attrs):
        if ":" in tag:
            tag = tag.split(":", 1)[-1]
        if tag == "link":
            attr_dict = dict(attrs)
            rel_tokens = (attr_dict.get("rel") or "").split()
            href = attr_dict.get("href")
            if href is not None and ("match" in rel_tokens or "mismatch" in rel_tokens):
                self.test_type = TestType.REF
                self.reference_paths.append(href)


def map_to_path(
    sources: list[ResourceAndType], wpt_base_url: str, is_resource=True, resource_path=None
) -> list[PathMapping]:
    filepaths: list[PathMapping] = []

    for source in sources:
        base_directory = test_type.input_path if source.type == ResourceType.INPUT else test_type.expected_path
        resource = resource_file_path(source.resource)

        if resource.startswith("/") or not is_resource:
            file_path = Path(base_directory, resource.lstrip("/"))
        else:
            assert resource_path is not None
            parsed_url = urlparse(source.resource)
            if parsed_url.scheme != "":
                print(f"Skipping '{source.resource}'. Downloading external resources is not supported.")
                continue
            # Add it as a sibling path if it's a relative resource
            sibling_location = Path(resource_path).parent
            parent_directory = Path(base_directory, sibling_location)

            file_path = Path(parent_directory, resource)
        # Map to source and destination
        output_path = wpt_base_url + str(file_path).replace(base_directory, "")

        filepaths.append(PathMapping(output_path, file_path.absolute()))

    return filepaths


def is_crash_test(url_string):
    # https://web-platform-tests.org/writing-tests/crashtest.html
    # A test file is treated as a crash test if they have -crash in their name before the file extension, or they are
    # located in a folder named crashtests
    parsed_url = urlparse(url_string)
    path_segments = parsed_url.path.strip("/").split("/")
    if len(path_segments) > 1 and "crashtests" in path_segments[::-1]:
        return True
    file_name = path_segments[-1]
    file_name_parts = file_name.split(".")
    return len(file_name_parts) > 1 and any(part.endswith("-crash") for part in file_name_parts[:-1])


def modify_sources(files, resources: list[ResourceAndType]) -> None:
    for file in files:
        # Get the distance to the wpt-imports folder
        folder_index = str(file).find(test_type.input_path)
        if folder_index == -1:
            folder_index = str(file).find(test_type.expected_path)
            non_prefixed_path = str(file)[folder_index + len(test_type.expected_path) :]
        else:
            non_prefixed_path = str(file)[folder_index + len(test_type.input_path) :]

        parent_folder_count = len(Path(non_prefixed_path).parent.parts) - 1
        parent_folder_path = "../" * parent_folder_count

        # Open the file in binary mode. Some WPT test files (e.g,
        # encoding-detection tests) are intentionally stored in a legacy
        # encoding rather than UTF-8. We only rewrite ASCII path references
        # below, so byte-level replacements preserve the original encoding of
        # the non-ASCII content.
        with open(file, "rb") as f:
            page_source = f.read()

        # Rewrite all absolute resource paths in a single pass, trying longer paths first, so that a path which contains
        # another (e.g. /common/blank.html?1 and /common/blank.html) is not rewritten twice.
        absolute_resources = {r.resource for r in resources if r.resource.startswith("/")}
        if absolute_resources:
            alternatives = b"|".join(
                re.escape(resource[1:].encode()) for resource in sorted(absolute_resources, key=len, reverse=True)
            )
            pattern = re.compile(b"/(" + alternatives + b")")
            page_source = pattern.sub(parent_folder_path.encode() + rb"\1", page_source)

        # Look for mentions of reference pages, and update their hrefs
        for raw_reference_path, reference_path in zip(raw_reference_paths, reference_paths):
            new_reference_path = parent_folder_path + "../../expected/wpt-import/" + reference_path[::]
            page_source = page_source.replace(raw_reference_path.encode(), new_reference_path.encode())

        with open(file, "wb") as f:
            f.write(page_source)


def normalize_url(url):
    parts = urlsplit(url)
    normalized_path = normpath(parts.path)
    normalized_url = urlunsplit((parts.scheme, parts.netloc, normalized_path, parts.query, parts.fragment))
    return normalized_url


def remove_repeated_url_slashes(url):
    parsed = urlparse(url)
    return "/" + "/".join(segment for segment in parsed.path.split("/") if segment)


def download_files(filepaths, wpt_base_url, skip_existing, required=True):
    downloaded_files = []

    for file in filepaths:
        normalized_path = remove_repeated_url_slashes(file.source)
        print(f"Source {normalized_path}, Destination {file.destination}")
        if normalized_path in download_exclude_list:
            print(f"Skipping {file.source} as it is in the exclude list")
            continue

        source = urljoin(wpt_base_url, normalized_path)
        destination = Path(os.path.normpath(file.destination))

        # A test and its reference page can share a relative resource, which then has to exist next to each of
        # them, so what has been downloaded is tracked by destination rather than by source.
        if destination in visited_destinations:
            continue
        visited_destinations.add(destination)

        if skip_existing and destination.exists():
            print(f"Skipping {destination} as it already exists")
            continue

        print(f"Downloading {source} to {destination}")

        try:
            connection = urlopen(source)
        except (HTTPError, InvalidURL, ValueError) as error:
            if required:
                raise
            print(f"Failed to download {file.source}: {error}")
            continue
        if connection.status != 200:
            print(f"Failed to download {file.source}")
            continue

        os.makedirs(destination.parent, exist_ok=True)

        with open(destination, "wb") as f:
            f.write(connection.read())

            downloaded_files.append(destination)

    return downloaded_files


def download_headers_files(filepaths, wpt_base_url, skip_existing):
    for file in filepaths:
        normalized_path = remove_repeated_url_slashes(file.source)
        headers_path = normalized_path + ".headers"
        source = urljoin(wpt_base_url, headers_path)
        destination = Path(os.path.normpath(str(file.destination) + ".headers"))

        if destination in visited_destinations:
            continue
        visited_destinations.add(destination)

        if skip_existing and destination.exists():
            print(f"Skipping {destination} as it already exists")
            continue

        try:
            connection = urlopen(source)
        except HTTPError:
            continue

        if connection.status != 200:
            continue

        os.makedirs(destination.parent, exist_ok=True)

        with open(destination, "wb") as f:
            f.write(connection.read())

        print(f"Downloaded headers file: {destination}")


def import_resources(
    resources: list[ResourceAndType],
    module_resources: set,
    worker_resources: set,
    script_path: str,
    base_path: str,
    wpt_base_url: str,
    skip_existing: bool,
    required=True,
) -> None:
    downloaded_scripts = []
    for resource in resources:
        referrer_path = script_path if resource.resource in module_resources else base_path
        paths = map_to_path([resource], wpt_base_url, True, referrer_path)
        downloaded_files = download_files(paths, wpt_base_url, skip_existing, required)
        download_headers_files(paths, wpt_base_url, skip_existing)

        for path in paths:
            file = Path(os.path.normpath(path.destination))
            if file not in downloaded_files or file.suffix not in [".js", ".mjs"]:
                continue
            downloaded_script_path = normpath(remove_repeated_url_slashes(path.source)).lstrip("/")
            downloaded_base_path = downloaded_script_path if resource.resource in worker_resources else base_path
            downloaded_scripts.append((file, downloaded_script_path, downloaded_base_path, resource.type))

    for file, downloaded_script_path, downloaded_base_path, resource_type in downloaded_scripts:
        finder = LinkedResourceFinder()
        with open(file, "rb") as f:
            finder.find_script_resources(f.read().decode("utf-8", errors="replace"))

        # URLs in a classic script resolve against the document or worker that runs it, not against the script, so only
        # a worker's own script has them rewritten. Module imports always resolve against the script itself.
        if downloaded_base_path == downloaded_script_path:
            rewritten_resources = finder.resources
        else:
            rewritten_resources = finder.module_resources
        if rewritten_resources:
            modify_sources([file], [ResourceAndType(resource, resource_type) for resource in rewritten_resources])

        import_resources(
            [ResourceAndType(resource, resource_type) for resource in finder.resources],
            finder.module_resources,
            finder.worker_resources,
            downloaded_script_path,
            downloaded_base_path,
            wpt_base_url,
            skip_existing,
            required=False,
        )


def create_expectation_files(files, skip_existing):
    # Ref tests don't have an expectation text file
    if test_type in [TestType.REF, TestType.CRASH]:
        return

    for file in files:
        new_path = str(file.destination).replace(test_type.input_path, test_type.expected_path)
        new_path = new_path.rsplit(".", 1)[0] + ".txt"

        expected_file = Path(new_path)
        if skip_existing and expected_file.exists():
            print(f"Skipping {expected_file} as it already exists")
            continue

        os.makedirs(expected_file.parent, exist_ok=True)
        expected_file.touch()


def main():
    parser = argparse.ArgumentParser(description="Import a WPT test into LibWeb")
    parser.add_argument("--force", action="store_true", help="Force download of files even if they already exist")
    parser.add_argument("--wpt-base-url", type=urlparse, default="https://wpt.live/")
    parser.add_argument(
        "--extra-resource",
        action="append",
        default=[],
        metavar="PATH",
        help="Also import a file that the test loads through a URL the importer cannot find, such as a worker script "
        "passed to a helper function. PATH is relative to the test, or to the WPT root if it starts with a slash. A "
        "script given this way is scanned as a worker script. May be repeated",
    )
    parser.add_argument("url", type=str, help="The URL of the WPT test to import")
    args = parser.parse_args()

    skip_existing = not args.force
    url_to_import = args.url
    wpt_base_url = args.wpt_base_url.geturl()
    print(f"Importing WPT test from: {wpt_base_url}")
    resource_path = "/".join(Path(url_to_import).parts[2::])

    with urlopen(url_to_import) as response:
        # Some WPT test files (e.g. `encoding-detection` tests) are intentionally stored in a
        # legacy encoding rather than UTF-8. We only parse the ASCII tag structure here, so
        # replacing undecodable bytes with U+FFFD is safe and avoids a UnicodeDecodeError.
        page = response.read().decode("utf-8", errors="replace")

    global test_type, raw_reference_paths
    if is_crash_test(url_to_import):
        test_type = TestType.CRASH
    else:
        identifier = TestTypeIdentifier(url_to_import)
        identifier.feed(page)
        test_type = identifier.test_type
        raw_reference_paths = identifier.reference_paths

    print(f"Identified {url_to_import} as type {test_type}, refs {raw_reference_paths}")

    main_file = [ResourceAndType(resource_path, ResourceType.INPUT)]
    main_paths = map_to_path(main_file, wpt_base_url, False)

    if test_type == TestType.REF and not raw_reference_paths:
        raise RuntimeError("Failed to file reference path in ref test")

    for raw_reference_path in raw_reference_paths:
        if raw_reference_path.startswith("/"):
            reference_path = raw_reference_path
            main_paths.append(
                PathMapping(
                    normalize_url(wpt_base_url + raw_reference_path),
                    Path(test_type.expected_path + raw_reference_path).absolute(),
                )
            )
        else:
            reference_path = Path(resource_path).parent.joinpath(raw_reference_path).__str__()
            main_paths.append(
                PathMapping(
                    normalize_url(wpt_base_url + "/" + reference_path),
                    Path(test_type.expected_path + "/" + reference_path).absolute(),
                )
            )
        reference_paths.append(reference_path)

    files_to_modify = download_files(main_paths, wpt_base_url, skip_existing)
    download_headers_files(main_paths, wpt_base_url, skip_existing)
    create_expectation_files(main_paths, skip_existing)

    input_parser = LinkedResourceFinder()
    input_parser.feed(page)
    additional_resources = [ResourceAndType(s, ResourceType.INPUT) for s in input_parser.resources]

    expected_parser = LinkedResourceFinder()
    for path in main_paths[1:]:
        with urlopen(path.source) as response:
            # Some WPT test files (e.g. `encoding-detection` tests) are intentionally stored in a
            # legacy encoding rather than UTF-8. We only parse the ASCII tag structure here, so
            # replacing undecodable bytes with U+FFFD is safe and avoids a UnicodeDecodeError.
            page = response.read().decode("utf-8", errors="replace")
            expected_parser.feed(page)
    additional_resources.extend(ResourceAndType(s, ResourceType.EXPECTED) for s in expected_parser.resources)
    additional_resources.extend(ResourceAndType(s, ResourceType.INPUT) for s in args.extra_resource)
    worker_resources = input_parser.worker_resources | expected_parser.worker_resources | set(args.extra_resource)

    modify_sources(files_to_modify, additional_resources)
    import_resources(
        additional_resources, set(), worker_resources, resource_path, resource_path, wpt_base_url, skip_existing
    )


if __name__ == "__main__":
    main()
