#
#    Copyright (c) 2024 Project CHIP Authors
#    All rights reserved.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#

import asyncio
import importlib
import logging
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Optional, List, Tuple, Dict, TYPE_CHECKING
from unittest.mock import MagicMock
from enum import Enum, IntFlag
from dataclasses import dataclass

from chip.clusters import Attribute
from chip.tracing import TracingContext
from mobly import signals
from mobly.test_runner import TestRunner
from chip.clusters import ClusterObjects
from chip.interaction_model import Status

# Import from matter_testing to avoid circular imports
# These will be imported by other modules from runner.py in the future
if TYPE_CHECKING:
    from chip.testing.matter_testing import (
        CommissionDeviceTest, MatterBaseTest, MatterTestConfig,
        generate_mobly_test_config, get_test_info, stash_globally,
        MatterStackState
    )
else:
    # At runtime, we'll use forward references
    CommissionDeviceTest = "chip.testing.matter_testing.CommissionDeviceTest"
    MatterBaseTest = "chip.testing.matter_testing.MatterBaseTest"
    MatterTestConfig = "chip.testing.matter_testing.MatterTestConfig"
    MatterStackState = "chip.testing.matter_testing.MatterStackState"
    # These functions will be imported at the point of use

# Export all the classes that were moved from matter_testing.py
__all__ = [
    'TestRunnerHooks', 'InternalTestRunnerHooks', 'AsyncMock', 'MockTestRunner',
    'run_tests_no_exit', 'run_tests',
    # Classes moved from matter_testing.py
    'TestStep', 'TestInfo', 'ProblemLocation', 'ProblemNotice', 'ProblemSeverity',
    'AttributePathLocation', 'ClusterMapper', 'ClusterPathLocation',
    'EventPathLocation', 'CommandPathLocation', 'FeaturePathLocation',
    'DeviceTypePathLocation', 'UnknownProblemLocation'
]


@dataclass
class TestStep:
    """Represents a step in a test case."""
    test_plan_number: int
    description: str


@dataclass
class TestInfo:
    """Information about a test case."""
    name: str
    steps: List[TestStep]
    desc: str
    pics: List[str]


class ClusterPathLocation:
    """Base class for problem locations in clusters."""

    def __init__(self, endpoint_id: int, cluster_id: int):
        self.endpoint_id = endpoint_id
        self.cluster_id = cluster_id

    def __str__(self) -> str:
        return f"Endpoint {self.endpoint_id}, Cluster {self.cluster_id}"


class AttributePathLocation(ClusterPathLocation):
    """Location of an attribute in a cluster."""

    def __init__(self, endpoint_id: int, cluster_id: int, attribute_id: int):
        super().__init__(endpoint_id, cluster_id)
        self.attribute_id = attribute_id

    def __str__(self) -> str:
        return f"{super().__str__()}, Attribute {self.attribute_id}"


class EventPathLocation(ClusterPathLocation):
    """Location of an event in a cluster."""

    def __init__(self, endpoint_id: int, cluster_id: int, event_id: int):
        super().__init__(endpoint_id, cluster_id)
        self.event_id = event_id

    def __str__(self) -> str:
        return f"{super().__str__()}, Event {self.event_id}"


class CommandPathLocation(ClusterPathLocation):
    """Location of a command in a cluster."""

    def __init__(self, endpoint_id: int, cluster_id: int, command_id: int):
        super().__init__(endpoint_id, cluster_id)
        self.command_id = command_id

    def __str__(self) -> str:
        return f"{super().__str__()}, Command {self.command_id}"


class FeaturePathLocation(ClusterPathLocation):
    """Location of a feature in a cluster."""

    def __init__(self, endpoint_id: int, cluster_id: int, feature_id: int):
        super().__init__(endpoint_id, cluster_id)
        self.feature_id = feature_id

    def __str__(self) -> str:
        return f"{super().__str__()}, Feature {self.feature_id}"


class DeviceTypePathLocation:
    """Location of a device type."""

    def __init__(self, endpoint_id: int, device_type_id: int):
        self.endpoint_id = endpoint_id
        self.device_type_id = device_type_id

    def __str__(self) -> str:
        return f"Endpoint {self.endpoint_id}, DeviceType {self.device_type_id}"


class UnknownProblemLocation:
    """Location for problems that don't have a specific location."""

    def __init__(self, description: str):
        self.description = description

    def __str__(self) -> str:
        return self.description


# Type alias for problem locations
ProblemLocation = Any  # Union of all location types


class ProblemSeverity(str, Enum):
    """Severity of a problem."""
    ERROR = "ERROR"
    WARNING = "WARNING"
    NOTE = "NOTE"


class ProblemNotice:
    """A notice about a problem in a test."""

    def __init__(self, test_name: str, location: ProblemLocation, severity: ProblemSeverity, problem: str, spec_location: str = ""):
        self.test_name = test_name
        self.location = location
        self.severity = severity
        self.problem = problem
        self.spec_location = spec_location

    def __str__(self) -> str:
        return f"{self.severity}: {self.location}: {self.problem}"


class ClusterMapper:
    """Maps cluster IDs to names and metadata."""

    def __init__(self, cluster_module):
        self.cluster_module = cluster_module
        self.cluster_id_to_name = {}
        self.cluster_id_to_obj = {}

        # Populate the mappings
        for name in dir(cluster_module):
            if name.startswith('_'):
                continue

            obj = getattr(cluster_module, name)
            if not isinstance(obj, type):
                continue

            if hasattr(obj, 'id'):
                self.cluster_id_to_name[obj.id] = name
                self.cluster_id_to_obj[obj.id] = obj


class TestRunnerHooks:
    """Base class for test runner hooks."""
    pass


class InternalTestRunnerHooks(TestRunnerHooks):
    """Default implementation of test runner hooks that logs to the console."""

    def start(self, count: int):
        logging.info(f'Starting test set, running {count} tests')

    def stop(self, duration: int):
        logging.info(f'Finished test set, ran for {duration}ms')

    def test_start(self, filename: str, name: str, count: int, steps: list[str] = []):
        logging.info(f'Starting test from {filename}: {name} - {count} steps')

    def test_stop(self, exception: Exception, duration: int):
        logging.info(f'Finished test in {duration}ms')

    def step_skipped(self, name: str, expression: str):
        # TODO: Do we really need the expression as a string? We can evaluate this in code very easily
        logging.info(f'\t\t**** Skipping: {name}')

    def step_start(self, name: str):
        # The way I'm calling this, the name is already includes the step number, but it seems like it might be good to separate these
        logging.info(f'\t\t***** Test Step {name}')

    def step_success(self, logger, logs, duration: int, request):
        pass

    def step_failure(self, logger, logs, duration: int, request, received):
        # TODO: there's supposed to be some kind of error message here, but I have no idea where it's meant to come from in this API
        logging.info('\t\t***** Test Failure : ')

    def step_unknown(self):
        """
        This method is called when the result of running a step is unknown. For example during a dry-run.
        """
        pass

    def show_prompt(self,
                    msg: str,
                    placeholder: Optional[str] = None,
                    default_value: Optional[str] = None) -> None:
        pass

    def test_skipped(self, filename: str, name: str):
        logging.info(f"Skipping test from {filename}: {name}")


class AsyncMock(MagicMock):
    async def __call__(self, *args, **kwargs):
        return super(AsyncMock, self).__call__(*args, **kwargs)


class MockTestRunner():

    def __init__(self, abs_filename: str, classname: str, test: str, endpoint: int = None,
                 pics: dict[str, bool] = None, paa_trust_store_path=None):
        # Import at runtime to avoid circular imports
        from chip.testing.matter_testing import MatterTestConfig, MatterStackState

        self.kvs_storage = 'kvs_admin.json'
        self.config = MatterTestConfig(endpoint=endpoint, paa_trust_store_path=paa_trust_store_path,
                                       pics=pics, storage_path=self.kvs_storage)
        self.set_test(abs_filename, classname, test)

        self.set_test_config(self.config)

        self.stack = MatterStackState(self.config)
        self.default_controller = self.stack.certificate_authorities[0].adminList[0].NewController(
            nodeId=self.config.controller_node_id,
            paaTrustStorePath=str(self.config.paa_trust_store_path),
            catTags=self.config.controller_cat_tags
        )

    def set_test(self, abs_filename: str, classname: str, test: str):
        self.test = test
        self.config.tests = [self.test]

        try:
            filename_path = Path(abs_filename)
            module = importlib.import_module(filename_path.stem)
        except ModuleNotFoundError:
            sys.path.append(str(filename_path.parent.resolve()))
            module = importlib.import_module(filename_path.stem)

        self.test_class = getattr(module, classname)

    def set_test_config(self, test_config: Any = None):
        # Import at runtime to avoid circular imports
        from chip.testing.matter_testing import MatterTestConfig

        if test_config is None:
            test_config = MatterTestConfig()

        self.config = test_config
        self.config.tests = [self.test]
        self.config.storage_path = self.kvs_storage
        if not self.config.dut_node_ids:
            self.config.dut_node_ids = [1]

    def Shutdown(self):
        self.stack.Shutdown()

    def run_test_with_mock_read(self, read_cache: Attribute.AsyncReadTransaction.ReadResponse, hooks=None):
        self.default_controller.Read = AsyncMock(return_value=read_cache)
        # This doesn't need to do anything since we are overriding the read anyway
        self.default_controller.FindOrEstablishPASESession = AsyncMock(return_value=None)
        self.default_controller.GetConnectedDevice = AsyncMock(return_value=None)
        with asyncio.Runner() as runner:
            # Import at runtime to avoid circular imports
            from chip.testing.matter_testing import run_tests_no_exit
            return run_tests_no_exit(self.test_class, self.config, runner.get_loop(),
                                     hooks, self.default_controller, self.stack)


def run_tests_no_exit(test_class: Any, matter_test_config: Any,
                      event_loop: asyncio.AbstractEventLoop, hooks: TestRunnerHooks,
                      default_controller=None, external_stack=None) -> bool:
    """
    Run Matter tests without exiting the process on failure.

    Args:
        test_class: The test class to run
        matter_test_config: Configuration for the test
        event_loop: Asyncio event loop to use
        hooks: Test runner hooks for callbacks
        default_controller: Optional controller to use
        external_stack: Optional external Matter stack to use

    Returns:
        True if all tests passed, False otherwise
    """
    # Import at runtime to avoid circular imports
    from chip.testing.matter_testing import CommissionDeviceTest, get_test_info, generate_mobly_test_config, stash_globally, MatterStackState

    # NOTE: It's not possible to pass event loop via Mobly TestRunConfig user params, because the
    #       Mobly deep copies the user params before passing them to the test class and the event
    #       loop is not serializable. So, we are setting the event loop as a test class member.
    CommissionDeviceTest.event_loop = event_loop
    test_class.event_loop = event_loop

    get_test_info(test_class, matter_test_config)

    # Load test config file.
    test_config = generate_mobly_test_config(matter_test_config)

    # Parse test specifiers if exist.
    tests = None
    if len(matter_test_config.tests) > 0:
        tests = matter_test_config.tests

    if external_stack:
        stack = external_stack
    else:
        stack = MatterStackState(matter_test_config)

    with TracingContext() as tracing_ctx:
        for destination in matter_test_config.trace_to:
            tracing_ctx.StartFromString(destination)

        test_config.user_params["matter_stack"] = stash_globally(stack)

        # TODO: Steer to right FabricAdmin!
        # TODO: If CASE Admin Subject is a CAT tag range, then make sure to issue NOC with that CAT tag
        if not default_controller:
            default_controller = stack.certificate_authorities[0].adminList[0].NewController(
                nodeId=matter_test_config.controller_node_id,
                paaTrustStorePath=str(matter_test_config.paa_trust_store_path),
                catTags=matter_test_config.controller_cat_tags,
                dacRevocationSetPath=str(matter_test_config.dac_revocation_set_path),
            )
        test_config.user_params["default_controller"] = stash_globally(default_controller)

        test_config.user_params["matter_test_config"] = stash_globally(matter_test_config)
        test_config.user_params["hooks"] = stash_globally(hooks)

        # Execute the test class with the config
        ok = True

        test_config.user_params["certificate_authority_manager"] = stash_globally(stack.certificate_authority_manager)

        # Execute the test class with the config
        ok = True

        runner = TestRunner(log_dir=test_config.log_path,
                            testbed_name=test_config.testbed_name)

        with runner.mobly_logger():
            if matter_test_config.commissioning_method is not None:
                runner.add_test_class(test_config, CommissionDeviceTest, None)

            # Add the tests selected unless we have a commission-only request
            if not matter_test_config.commission_only:
                runner.add_test_class(test_config, test_class, tests)

            if hooks:
                # Right now, we only support running a single test class at once,
                # but it's relatively easy to expand that to make the test process faster
                # TODO: support a list of tests
                hooks.start(count=1)
                # Mobly gives the test run time in seconds, lets be a bit more precise
                runner_start_time = datetime.now(timezone.utc)

            try:
                runner.run()
                ok = runner.results.is_all_pass and ok
                if matter_test_config.fail_on_skipped_tests and runner.results.skipped:
                    ok = False
            except TimeoutError:
                ok = False
            except signals.TestAbortAll:
                ok = False
            except Exception:
                logging.exception('Exception when executing %s.', test_config.testbed_name)
                ok = False

    if hooks:
        duration = (datetime.now(timezone.utc) - runner_start_time) / timedelta(microseconds=1)
        hooks.stop(duration=duration)

    if not external_stack:
        async def shutdown():
            stack.Shutdown()
        # Shutdown the stack when all done. Use the async runner to ensure that
        # during the shutdown callbacks can use tha same async context which was used
        # during the initialization.
        event_loop.run_until_complete(shutdown())

    if ok:
        logging.info("Final result: PASS !")
    else:
        logging.error("Final result: FAIL !")
    return ok


def run_tests(test_class: Any, matter_test_config: Any,
              hooks: TestRunnerHooks, default_controller=None, external_stack=None) -> None:
    """
    Run Matter tests and exit the process with code 1 if any test fails.

    Args:
        test_class: The test class to run
        matter_test_config: Configuration for the test
        hooks: Test runner hooks for callbacks
        default_controller: Optional controller to use
        external_stack: Optional external Matter stack to use
    """
    with asyncio.Runner() as runner:
        if not run_tests_no_exit(test_class, matter_test_config, runner.get_loop(),
                                 hooks, default_controller, external_stack):
            sys.exit(1)
