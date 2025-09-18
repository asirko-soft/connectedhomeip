# Copyright (c) 2025 Project CHIP Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import asyncio
import logging
import os
import subprocess
import tempfile
import threading
from dataclasses import dataclass, field
from typing import List, Optional, Union

import matter.clusters as Clusters
from matter import ChipDeviceCtrl

logger = logging.getLogger(__name__)


@dataclass
class OtaFileSource:
    """Defines the source for the OTA image(s)."""
    # Path to a single .ota file or a .txt file listing multiple images
    path: str

    def get_args(self) -> List[str]:
        """Returns the command-line arguments for this source."""
        arg = "--otaImageList" if self.path.endswith(".txt") else "--filepath"
        return [arg, self.path]


@dataclass
class OTARequestorConfig:
    """Configuration for the OTA Requestor application."""
    discriminator: int
    app_path: str = "./out/debug/chip-ota-requestor-app"
    passcode: int = 20202021
    port: int = 5542
    vendor_id: Optional[int] = 65521
    product_id: Optional[int] = 32769
    extra_args: List[str] = field(default_factory=list)

    def build_command(self, kvs_path: str) -> List[str]:
        cmd = [
            self.app_path,
            "--KVS", kvs_path,
            "--discriminator", str(self.discriminator),
            "--passcode", str(self.passcode),
            "--secured-device-port", str(self.port),
        ]
        if self.vendor_id:
            cmd.extend(["--vendor-id", str(self.vendor_id)])
        if self.product_id:
            cmd.extend(["--product-id", str(self.product_id)])
        cmd.extend(self.extra_args)
        return cmd


@dataclass
class OTAProviderConfig:
    """Configuration for the OTA Provider application."""
    discriminator: int
    ota_source: OtaFileSource
    app_path: str = "./out/debug/chip-ota-provider-app"
    passcode: int = 20202021
    port: int = 5541
    # Optional provider flags (emit only if provided)
    image_uri: Optional[str] = None                       # --imageUri
    apply_update_action: Optional[str] = None             # --applyUpdateAction
    user_consent_needed: bool = False                     # --userConsentNeeded (no arg)
    user_consent_state: Optional[str] = None              # --userConsentState
    query_image_status: Optional[str] = None              # --queryImageStatus
    delayed_apply_action_time_sec: Optional[int] = None   # --delayedApplyActionTimeSec
    delayed_query_action_time_sec: Optional[int] = None   # --delayedQueryActionTimeSec
    ignore_query_image: Optional[str] = None              # --ignoreQueryImage
    ignore_apply_update: Optional[str] = None             # --ignoreApplyUpdate
    poll_interval: Optional[int] = None                   # --pollInterval
    max_bdx_block_size: Optional[int] = None              # --maxBDXBlockSize
    extra_args: List[str] = field(default_factory=list)

    def build_command(self, kvs_path: str) -> List[str]:
        cmd = [
            self.app_path,
            "--KVS", kvs_path,
            "--discriminator", str(self.discriminator),
            "--passcode", str(self.passcode),
            "--secured-device-port", str(self.port),
        ]
        # Source for images
        cmd.extend(self.ota_source.get_args())
        # Optional provider flags
        if self.image_uri:
            cmd.extend(["--imageUri", self.image_uri])
        if self.apply_update_action:
            cmd.extend(["--applyUpdateAction", str(self.apply_update_action)])
        if self.user_consent_needed:
            cmd.append("--userConsentNeeded")
        if self.user_consent_state:
            cmd.extend(["--userConsentState", str(self.user_consent_state)])
        if self.query_image_status:
            cmd.extend(["--queryImageStatus", self.query_image_status])
        if self.delayed_apply_action_time_sec is not None:
            cmd.extend(["--delayedApplyActionTimeSec", str(self.delayed_apply_action_time_sec)])
        if self.delayed_query_action_time_sec is not None:
            cmd.extend(["--delayedQueryActionTimeSec", str(self.delayed_query_action_time_sec)])
        if self.ignore_query_image:
            cmd.extend(["--ignoreQueryImage", str(self.ignore_query_image)])
        if self.ignore_apply_update:
            cmd.extend(["--ignoreApplyUpdate", str(self.ignore_apply_update)])
        if self.poll_interval is not None:
            cmd.extend(["--pollInterval", str(self.poll_interval)])
        if self.max_bdx_block_size is not None:
            cmd.extend(["--maxBDXBlockSize", str(self.max_bdx_block_size)])
        cmd.extend(self.extra_args)
        return cmd


class AppSubprocess:
    """A generic wrapper for managing an application subprocess."""

    def __init__(self, config: Union[OTAProviderConfig, OTARequestorConfig], log_prefix: str):
        self._config = config
        self._log_prefix = log_prefix
        self._kvs_file = tempfile.NamedTemporaryFile(delete=False)
        self._proc: Optional[subprocess.Popen] = None
        self._log_thread: Optional[threading.Thread] = None
        self._stop_logging = threading.Event()
        self.ready_event = threading.Event()

    def _log_output(self, wait_for_log: Optional[str]):
        """Reads and logs output from the subprocess stdout."""
        for line in iter(self._proc.stdout.readline, ''):
            if self._stop_logging.is_set():
                break
            logger.info(f"{self._log_prefix} {line.strip()}")
            if wait_for_log and wait_for_log in line and not self.ready_event.is_set():
                logger.info(f"Found startup log: '{wait_for_log}'. App is ready.")
                self.ready_event.set()
        if self._proc.stdout:
            self._proc.stdout.close()

    def start(self, wait_for_log: Optional[str] = None, timeout: int = 30):
        """Starts the application subprocess and waits for it to be ready."""
        command = self._config.build_command(self._kvs_file.name)
        logger.info(f"Starting app with command: {' '.join(command)}")
        # Close the temp file handle before spawning the process (avoid sharing issues)
        try:
            self._kvs_file.close()
        except Exception:
            pass
        self._proc = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )

        self._log_thread = threading.Thread(target=self._log_output, args=(wait_for_log,))
        self._log_thread.daemon = True
        self._log_thread.start()

        if wait_for_log:
            ready = self.ready_event.wait(timeout)
            if not ready:
                self.stop()
                raise TimeoutError(f"App did not emit '{wait_for_log}' within {timeout}s.")

    def stop(self):
        """Stops the application subprocess and cleans up resources."""
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
            logger.info(f"Terminated app process {self._proc.pid}")

        if self._log_thread:
            self._stop_logging.set()
            self._log_thread.join(timeout=2)

        try:
            os.remove(self._kvs_file.name)
        except FileNotFoundError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.stop()


class OTARequestorSubprocess(AppSubprocess):
    """Manages the lifecycle of an OTA Requestor app."""

    def __init__(self, config: OTARequestorConfig):
        super().__init__(config, "[REQUESTOR]")


class OTAProviderSubprocess(AppSubprocess):
    """
    Manages the lifecycle of an OTA Provider app,
    including optional commissioning and cleanup.
    """

    def __init__(self, config: OTAProviderConfig):
        super().__init__(config, "[PROVIDER]")
        self.node_id: Optional[int] = None
        self._original_acls = {}
        self.is_commissioned = False

    async def commission(self, controller: ChipDeviceCtrl, node_id: int):
        """Commissions the provider onto the network. Skips if already commissioned."""
        if self.is_commissioned:
            logger.warning(f"Provider {node_id} is already commissioned. Skipping.")
            return

        self.node_id = node_id
        logger.info(f"Commissioning OTA Provider to node ID {self.node_id}")
        await controller.CommissionOnNetwork(
            nodeId=self.node_id,
            setupPinCode=self._config.passcode,
            filterType=ChipDeviceCtrl.DiscoveryFilterType.LONG_DISCRIMINATOR,
            filter=self._config.discriminator
        )
        self.is_commissioned = True

    async def setup_acls(self, controller: ChipDeviceCtrl, requestor_node_id: int):
        """Backs up existing ACLs and writes new ones to allow OTA communication."""
        if not self.node_id:
            raise ValueError("Provider node_id must be set before setting up ACLs.")

        logger.info(f"Setting up ACLs between Provider ({self.node_id}) and Requestor ({requestor_node_id})")

        if requestor_node_id not in self._original_acls:
            self._original_acls[requestor_node_id] = await controller.ReadAttribute(requestor_node_id, [(0, Clusters.AccessControl.Attributes.Acl)])
        if self.node_id not in self._original_acls:
            self._original_acls[self.node_id] = await controller.ReadAttribute(self.node_id, [(0, Clusters.AccessControl.Attributes.Acl)])

        requestor_acl_list = self._original_acls[requestor_node_id][0][0].Data + [
            Clusters.AccessControl.Structs.AccessControlEntryStruct(
                privilege=Clusters.AccessControl.Enums.AccessControlEntryPrivilegeEnum.kOperate,
                authMode=Clusters.AccessControl.Enums.AccessControlEntryAuthModeEnum.kCase,
                subjects=[self.node_id],
                targets=[Clusters.AccessControl.Structs.AccessControlTargetStruct(
                    cluster=Clusters.OtaSoftwareUpdateRequestor.id, endpoint=0)]
            )]
        await controller.WriteAttribute(requestor_node_id, [(0, Clusters.AccessControl.Attributes.Acl(requestor_acl_list))])

        provider_acl_list = self._original_acls[self.node_id][0][0].Data + [
            Clusters.AccessControl.Structs.AccessControlEntryStruct(
                privilege=Clusters.AccessControl.Enums.AccessControlEntryPrivilegeEnum.kOperate,
                authMode=Clusters.AccessControl.Enums.AccessControlEntryAuthModeEnum.kCase,
                subjects=[requestor_node_id],
                targets=[Clusters.AccessControl.Structs.AccessControlTargetStruct(
                    cluster=Clusters.OtaSoftwareUpdateProvider.id, endpoint=0)]
            )]
        await controller.WriteAttribute(self.node_id, [(0, Clusters.AccessControl.Attributes.Acl(provider_acl_list))])

    async def commission_and_setup_acls(self, controller: ChipDeviceCtrl, node_id: int, requestor_node_id: int):
        """Convenience method to run commissioning and ACL setup in sequence."""
        await self.commission(controller, node_id)
        await self.setup_acls(controller, requestor_node_id)

    async def shutdown(self, controller: ChipDeviceCtrl, requestor_node_id: int, restore_acls: bool = True):
        """Performs a comprehensive shutdown, with optional ACL restoration."""
        logger.info(f"Shutting down provider {self.node_id} and cleaning up resources.")

        if restore_acls:
            logger.info("Restoring original ACLs.")
            if self.node_id in self._original_acls:
                await controller.WriteAttribute(self.node_id, [(0, Clusters.AccessControl.Attributes.Acl(self._original_acls[self.node_id][0][0].Data))])
            if requestor_node_id in self._original_acls:
                await controller.WriteAttribute(requestor_node_id, [(0, Clusters.AccessControl.Attributes.Acl(self._original_acls[requestor_node_id][0][0].Data))])

        if self.node_id:
            controller.ExpireSessions(self.node_id)

        self.stop()
