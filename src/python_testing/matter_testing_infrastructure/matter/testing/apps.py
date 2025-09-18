# Copyright (c) 2024 Project CHIP Authors
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

import os
import signal
import tempfile
from dataclasses import dataclass
from typing import Optional, Union

import matter.clusters as Clusters
from matter.ChipDeviceCtrl import ChipDeviceController
from matter.clusters.Types import NullValue
from matter.testing.tasks import Subprocess


@dataclass
class OtaImagePath:
    """Represents a path to a single OTA image file."""
    path: str

    @property
    def ota_args(self) -> list[str]:
        """Return the command line arguments for this OTA image path."""
        return ["--filepath", self.path]


@dataclass
class ImageListPath:
    """Represents a path to a file containing a list of OTA images."""
    path: str

    @property
    def ota_args(self) -> list[str]:
        """Return the command line arguments for this image list path."""
        return ["--otaImageList", self.path]


class AppServerSubprocess(Subprocess):
    """Wrapper class for starting an application server in a subprocess."""

    # Prefix for log messages from the application server.
    PREFIX = b"[SERVER]"

    def __init__(self, app: str, storage_dir: str, discriminator: int,
                 passcode: int, port: int = 5540, extra_args: list[str] = []):
        # Create a temporary KVS file and keep the descriptor to avoid leaks.
        self.kvs_fd, kvs_path = tempfile.mkstemp(dir=storage_dir, prefix="kvs-app-")
        try:
            # Build the command list
            command = [app]
            if extra_args:
                command.extend(extra_args)

            command.extend([
                "--KVS", kvs_path,
                '--secured-device-port', str(port),
                "--discriminator", str(discriminator),
                "--passcode", str(passcode)
            ])

            # Start the server application
            super().__init__(*command,  # Pass the constructed command list
                             output_cb=lambda line, is_stderr: self.PREFIX + line)
        except Exception:
            # Do not leak KVS file descriptor on failure
            os.close(self.kvs_fd)
            raise

    def __del__(self):
        # Do not leak KVS file descriptor.
        if hasattr(self, "kvs_fd"):
            try:
                os.close(self.kvs_fd)
            except OSError:
                pass


class IcdAppServerSubprocess(AppServerSubprocess):
    """Wrapper class for starting an ICD application server in a subprocess."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.paused = False

    def pause(self, check_state: bool = True):
        if check_state and self.paused:
            raise ValueError("ICD TH Server unexpectedly is already paused")
        if not self.paused:
            # Stop (halt) the ICD server process by sending a SIGTOP signal.
            self.p.send_signal(signal.SIGSTOP)
            self.paused = True

    def resume(self, check_state: bool = True):
        if check_state and not self.paused:
            raise ValueError("ICD TH Server unexpectedly is already running")
        if self.paused:
            # Resume (continue) the ICD server process by sending a SIGCONT signal.
            self.p.send_signal(signal.SIGCONT)
            self.paused = False

    def terminate(self):
        # Make sure the ICD server process is not paused before terminating it.
        self.resume(check_state=False)
        super().terminate()


class JFControllerSubprocess(Subprocess):
    """Wrapper class for starting a controller in a subprocess."""

    # Prefix for log messages from the application server.
    PREFIX = b"[JF-CTRL]"

    def __init__(self, app: str, rpc_server_port: int, storage_dir: str,
                 vendor_id: int, extra_args: list[str] = []):

        # Build the command list
        command = [app]
        if extra_args:
            command.extend(extra_args)

        command.extend([
            "--rpc-server-port", str(rpc_server_port),
            "--storage-directory", storage_dir,
            "--commissioner-vendor-id", str(vendor_id)
        ])

        # Start the server application
        super().__init__(*command,  # Pass the constructed command list
                         output_cb=lambda line, is_stderr: self.PREFIX + line)


class OTARequestorSubprocess(AppServerSubprocess):
    """Wrapper class for starting an OTA Requestor application server in a subprocess."""

    # Prefix for log messages from the OTA requestor application.
    PREFIX = b"[OTA-REQUESTOR]"

    def __init__(self, app: str, storage_dir: str, discriminator: int,
                 passcode: int, port: int = 5542, extra_args: list[str] = []):
        super().__init__(app=app, storage_dir=storage_dir, discriminator=discriminator,
                         passcode=passcode, port=port, extra_args=extra_args)


class OTAProviderSubprocess(AppServerSubprocess):
    """Wrapper class for starting an OTA Provider application server in a subprocess."""

    DEFAULT_ADMIN_NODE_ID = 112233

    # Prefix for log messages from the OTA provider application.
    PREFIX = b"[OTA-PROVIDER]"

    def __init__(self, app: str, storage_dir: str, discriminator: int,
                 passcode: int, ota_source: Union[OtaImagePath, ImageListPath],
                 port: int = 5541, extra_args: list[str] = [],
                 image_uri: Optional[str] = None,
                 apply_update_action: Optional[str] = None,
                 user_consent_needed: bool = False,
                 user_consent_state: Optional[str] = None,
                 query_image_status: Optional[str] = None,
                 delayed_apply_action_time_sec: Optional[int] = None,
                 delayed_query_action_time_sec: Optional[int] = None,
                 ignore_query_image: Optional[str] = None,
                 ignore_apply_update: Optional[str] = None,
                 poll_interval: Optional[int] = None,
                 max_bdx_block_size: Optional[int] = None):
        """Initialize the OTA Provider subprocess.

        Args:
            app: Path to the chip-ota-provider-app executable
            storage_dir: Directory for persistent storage
            discriminator: Discriminator for commissioning
            passcode: Passcode for commissioning
            ota_source: Either OtaImagePath or ImageListPath specifying the OTA image source
            port: UDP port for secure connections (default: 5541)
            extra_args: Additional command line arguments
            image_uri: Overrides image URI advertised by provider (emits --imageUri)
            apply_update_action: Apply update action (emits --applyUpdateAction)
            user_consent_needed: If True, emit --userConsentNeeded (no argument)
            user_consent_state: User consent state (emits --userConsentState)
            query_image_status: Query image status (emits --queryImageStatus)
            delayed_apply_action_time_sec: Delay for apply action (emits --delayedApplyActionTimeSec)
            delayed_query_action_time_sec: Delay for query action (emits --delayedQueryActionTimeSec)
            ignore_query_image: Ignore query image setting (emits --ignoreQueryImage)
            ignore_apply_update: Ignore apply update setting (emits --ignoreApplyUpdate)
            poll_interval: Poll interval (emits --pollInterval)
            max_bdx_block_size: Max BDX block size (emits --maxBDXBlockSize)
        """

        # Build OTA-specific arguments using the ota_source and optional provider flags
        provider_args: list[str] = []
        if image_uri:
            provider_args.extend(["--imageUri", image_uri])
        if apply_update_action:
            provider_args.extend(["--applyUpdateAction", str(apply_update_action)])
        if user_consent_needed:
            provider_args.append("--userConsentNeeded")
        if user_consent_state:
            provider_args.extend(["--userConsentState", str(user_consent_state)])
        if query_image_status:
            provider_args.extend(["--queryImageStatus", str(query_image_status)])
        if delayed_apply_action_time_sec is not None:
            provider_args.extend(["--delayedApplyActionTimeSec", str(delayed_apply_action_time_sec)])
        if delayed_query_action_time_sec is not None:
            provider_args.extend(["--delayedQueryActionTimeSec", str(delayed_query_action_time_sec)])
        if ignore_query_image:
            provider_args.extend(["--ignoreQueryImage", str(ignore_query_image)])
        if ignore_apply_update:
            provider_args.extend(["--ignoreApplyUpdate", str(ignore_apply_update)])
        if poll_interval is not None:
            provider_args.extend(["--pollInterval", str(poll_interval)])
        if max_bdx_block_size is not None:
            provider_args.extend(["--maxBDXBlockSize", str(max_bdx_block_size)])

        combined_extra_args = ota_source.ota_args + provider_args + extra_args

        # Initialize with the combined arguments
        super().__init__(app=app, storage_dir=storage_dir, discriminator=discriminator,
                         passcode=passcode, port=port, extra_args=combined_extra_args)

    def create_acl_entry(self, dev_ctrl: ChipDeviceController, provider_node_id: int, requestor_node_id: Optional[int] = None):
        """Create ACL entries to allow OTA requestors to access the provider.

        Args:
            dev_ctrl: Device controller for sending commands
            provider_node_id: Node ID of the OTA provider
            requestor_node_id: Optional specific requestor node ID for targeted access

        Returns:
            Result of the ACL write operation
        """
        # Standard ACL entry for OTA Provider cluster
        admin_node_id = dev_ctrl.nodeId if hasattr(dev_ctrl, 'nodeId') else self.DEFAULT_ADMIN_NODE_ID
        requestor_subjects = [requestor_node_id] if requestor_node_id else NullValue

        # Create ACL entries using proper struct constructors
        acl_entries = [
            # Admin entry
            Clusters.AccessControl.Structs.AccessControlEntryStruct(  # type: ignore
                privilege=Clusters.AccessControl.Enums.AccessControlEntryPrivilegeEnum.kAdminister,  # type: ignore
                authMode=Clusters.AccessControl.Enums.AccessControlEntryAuthModeEnum.kCase,  # type: ignore
                subjects=[admin_node_id],  # type: ignore
                targets=NullValue
            ),
            # Operate entry
            Clusters.AccessControl.Structs.AccessControlEntryStruct(  # type: ignore
                privilege=Clusters.AccessControl.Enums.AccessControlEntryPrivilegeEnum.kOperate,  # type: ignore
                authMode=Clusters.AccessControl.Enums.AccessControlEntryAuthModeEnum.kCase,  # type: ignore
                subjects=requestor_subjects,  # type: ignore
                targets=[
                    Clusters.AccessControl.Structs.AccessControlTargetStruct(  # type: ignore
                        cluster=Clusters.OtaSoftwareUpdateProvider.id,  # type: ignore
                        endpoint=NullValue,
                        deviceType=NullValue
                    )
                ],
            )
        ]

        # Create the attribute descriptor for the ACL attribute
        acl_attribute = Clusters.AccessControl.Attributes.Acl(acl_entries)

        return dev_ctrl.WriteAttribute(
            nodeid=provider_node_id,
            attributes=[(0, acl_attribute)]
        )
