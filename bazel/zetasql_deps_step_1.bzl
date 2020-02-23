#
# Copyright 2019 ZetaSQL Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

""" Step 1 to load ZetaSQL dependencies. """

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# rules_foreign_cc and grpc (transitively) both require a "bazel_version" repo
# but depend on them being something different. So we have to override them both
# by defining the repo first.
load("@com_google_zetasql//bazel:zetasql_bazel_version.bzl", "zetasql_bazel_version")

def zetasql_deps_step_1():
    zetasql_bazel_version()

    if not native.existing_rule("rules_foreign_cc"):
        http_archive(
            name = "rules_foreign_cc",
            strip_prefix = "rules_foreign_cc-8b477ca9cb248fc472f152aa1a44c55ab71c4636",
            urls = [
                "https://github.com/bazelbuild/rules_foreign_cc/archive/8b477ca9cb248fc472f152aa1a44c55ab71c4636.tar.gz",
            ],
            sha256 = "c7cd62dc965ee3ee4f513fb6ce1d8d0d3dfcbb6ed5baa138bbea62d9dd5653fd",
        )
