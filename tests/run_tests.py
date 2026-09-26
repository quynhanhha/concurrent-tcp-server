#!/usr/bin/env python3
"""Self-contained tests for the protocol helpers, client, and server."""

from __future__ import annotations

import contextlib
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = ROOT / "tests"

MSG_JOIN = 1
MSG_JOIN_ACCEPTED = 2
MSG_JOIN_REJECTED = 3
MSG_GAME_READY = 4
MSG_SHIP_SUBMIT = 5
MSG_SHIP_RESULT = 6
MSG_MOVE_SUBMIT = 7
MSG_MOVE_RESULT = 8
MSG_OPPONENT_MOVE = 9
MSG_EXT_MOVE_SUBMIT = 10
MSG_EXT_MOVE_RESULT = 11
MSG_ERROR = 12

STATUS_OK = 0
STATUS_FAIL = 1
STATUS_PROTOCOL_ERROR = 2
STATUS_GAME_FULL = 3
STATUS_ENGINE_FAIL = 5
STATUS_DISCONNECTED = 6


def frame(message_type: int, status: int = STATUS_OK,
          payload: bytes = b"") -> bytes:
    return bytes((message_type, status)) + len(payload).to_bytes(2, "big") + payload


def send_frame(sock: socket.socket, message_type: int, status: int = STATUS_OK,
               payload: bytes = b"") -> None:
    sock.sendall(frame(message_type, status, payload))


def recv_exact(sock: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise AssertionError("connection closed before complete frame")
        data.extend(chunk)
    return bytes(data)


def recv_frame(sock: socket.socket) -> tuple[int, int, bytes]:
    header = recv_exact(sock, 4)
    length = int.from_bytes(header[2:], "big")
    return header[0], header[1], recv_exact(sock, length) if length else b""


def assert_frame(test: unittest.TestCase, actual: tuple[int, int, bytes],
                 message_type: int, status: int = STATUS_OK,
                 payload: bytes = b"") -> None:
    test.assertEqual(actual, (message_type, status, payload))


def ships_payload() -> bytes:
    records = ((b"A1", 2, 0), (b"B2", 3, 1),
               (b"C3", 4, 0), (b"D4", 5, 1))
    return b"".join(coordinate.ljust(4, b"\0") + bytes((length, direction))
                     for coordinate, length, direction in records)


@contextlib.contextmanager
def running_server(binary: Path, **settings: str):
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    environment = os.environ.copy()
    environment.update(settings)
    process = subprocess.Popen(
        [str(binary), str(port)],
        cwd=ROOT,
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        for _ in range(100):
            if process.poll() is not None:
                raise AssertionError(f"server exited with {process.returncode}")
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.05).close()
                break
            except OSError:
                time.sleep(0.01)
        else:
            raise AssertionError("server did not start listening")
        yield port
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def join_client(port: int, game_id: int) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
    send_frame(sock, MSG_JOIN, payload=game_id.to_bytes(4, "big"))
    actual = recv_frame(sock)
    if actual != (MSG_JOIN_ACCEPTED, STATUS_OK, b""):
        sock.close()
        raise AssertionError(f"join rejected: {actual}")
    return sock


def join_pair(port: int, game_id: int) -> tuple[socket.socket, socket.socket]:
    player_one = join_client(port, game_id)
    player_two = join_client(port, game_id)
    assert recv_frame(player_one) == (MSG_GAME_READY, STATUS_OK, b"")
    assert recv_frame(player_two) == (MSG_GAME_READY, STATUS_OK, b"")
    return player_one, player_two


class RepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tempdir = tempfile.TemporaryDirectory(prefix="project2-tests-")
        build = Path(cls.tempdir.name)
        compiler = os.environ.get("CC", "cc")
        common_test = build / "test_common"
        server = build / "server-test"
        client_probe = build / "client-probe"

        commands = (
            [compiler, "-Wall", "-Wextra", "-Isrc", str(TESTS / "test_common.c"),
             str(SRC / "common.c"), "-o", str(common_test)],
            [compiler, "-Wall", "-Wextra", "-pthread", "-Isrc",
             str(SRC / "server.c"), str(SRC / "server_game.c"),
             str(SRC / "common.c"), str(TESTS / "fake_engine.c"),
             "-o", str(server)],
            [compiler, "-Wall", "-Wextra", "-pthread", "-Isrc",
             str(TESTS / "client_probe.c"), str(SRC / "client.c"),
             str(SRC / "common.c"), "-o", str(client_probe)],
        )
        for command in commands:
            result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                    text=True, check=False)
            if result.returncode != 0:
                raise RuntimeError(
                    "compile failed:\n" + " ".join(command) +
                    "\n" + result.stdout + result.stderr
                )
        cls.common_test = common_test
        cls.server = server
        cls.client_probe = client_probe

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tempdir.cleanup()

    def test_common_protocol_helpers(self) -> None:
        result = subprocess.run([str(self.common_test)], capture_output=True,
                                text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_full_game_flow_including_extended_move(self) -> None:
        with running_server(self.server) as port:
            player_one, player_two = join_pair(port, 100)
            try:
                send_frame(player_one, MSG_SHIP_SUBMIT, payload=ships_payload())
                send_frame(player_two, MSG_SHIP_SUBMIT, payload=ships_payload())
                assert_frame(self, recv_frame(player_one), MSG_SHIP_RESULT,
                             payload=b"\x01")
                assert_frame(self, recv_frame(player_two), MSG_SHIP_RESULT,
                             payload=b"\x02")

                send_frame(player_one, MSG_MOVE_SUBMIT, payload=b"A1\0\0")
                assert_frame(self, recv_frame(player_one), MSG_MOVE_RESULT,
                             payload=b"\x01")
                assert_frame(self, recv_frame(player_two), MSG_OPPONENT_MOVE,
                             payload=b"A1\0\0\x01")

                send_frame(player_two, MSG_EXT_MOVE_SUBMIT, payload=b"C1\0\0")
                extended = recv_frame(player_two)
                self.assertEqual(extended[0:2], (MSG_EXT_MOVE_RESULT, STATUS_OK))
                self.assertEqual(extended[2][0], 4)
                assert_frame(self, recv_frame(player_one), MSG_OPPONENT_MOVE,
                             payload=b"C1\0\0\x04")

                send_frame(player_one, MSG_MOVE_SUBMIT, payload=b"B1\0\0")
                assert_frame(self, recv_frame(player_one), MSG_MOVE_RESULT,
                             payload=b"\x02")
                assert_frame(self, recv_frame(player_two), MSG_OPPONENT_MOVE,
                             payload=b"B1\0\0\x02")
            finally:
                player_one.close()
                player_two.close()

    def test_multiple_games_can_be_active(self) -> None:
        with running_server(self.server) as port:
            pairs = []
            try:
                for game_id in (201, 202):
                    pairs.append(join_pair(port, game_id))
                self.assertEqual(len(pairs), 2)
            finally:
                for pair in pairs:
                    for sock in pair:
                        sock.close()

    def test_active_game_rejects_third_client(self) -> None:
        with running_server(self.server) as port:
            player_one, player_two = join_pair(port, 300)
            try:
                third = socket.create_connection(("127.0.0.1", port), timeout=2)
                try:
                    send_frame(third, MSG_JOIN, payload=(300).to_bytes(4, "big"))
                    assert_frame(self, recv_frame(third), MSG_JOIN_REJECTED,
                                 STATUS_GAME_FULL)
                finally:
                    third.close()
            finally:
                player_one.close()
                player_two.close()

    def test_protocol_error_ends_game(self) -> None:
        with running_server(self.server) as port:
            player_one, player_two = join_pair(port, 400)
            try:
                send_frame(player_one, MSG_MOVE_SUBMIT, payload=b"A1\0\0")
                assert_frame(self, recv_frame(player_one), MSG_ERROR,
                             STATUS_PROTOCOL_ERROR)
                assert_frame(self, recv_frame(player_two), MSG_ERROR,
                             STATUS_PROTOCOL_ERROR)
            finally:
                player_one.close()
                player_two.close()

    def test_non_join_message_is_rejected(self) -> None:
        with running_server(self.server) as port:
            sock = socket.create_connection(("127.0.0.1", port), timeout=2)
            try:
                send_frame(sock, MSG_MOVE_SUBMIT, payload=b"A1\0\0")
                assert_frame(self, recv_frame(sock), MSG_ERROR,
                             STATUS_PROTOCOL_ERROR)
            finally:
                sock.close()

    def test_engine_failure_is_reported(self) -> None:
        with running_server(self.server, FAKE_PLACE_FAIL="1") as port:
            player_one, player_two = join_pair(port, 500)
            try:
                send_frame(player_one, MSG_SHIP_SUBMIT, payload=ships_payload())
                send_frame(player_two, MSG_SHIP_SUBMIT, payload=ships_payload())
                assert_frame(self, recv_frame(player_one), MSG_ERROR,
                             STATUS_ENGINE_FAIL)
                assert_frame(self, recv_frame(player_two), MSG_ERROR,
                             STATUS_ENGINE_FAIL)
            finally:
                player_one.close()
                player_two.close()

    def test_normal_turn_failure_is_reported(self) -> None:
        with running_server(self.server, FAKE_TURN_FAIL="1") as port:
            player_one, player_two = join_pair(port, 550)
            try:
                send_frame(player_one, MSG_SHIP_SUBMIT, payload=ships_payload())
                send_frame(player_two, MSG_SHIP_SUBMIT, payload=ships_payload())
                recv_frame(player_one)
                recv_frame(player_two)
                send_frame(player_one, MSG_MOVE_SUBMIT, payload=b"A1\0\0")
                assert_frame(self, recv_frame(player_one), MSG_ERROR,
                             STATUS_ENGINE_FAIL)
                assert_frame(self, recv_frame(player_two), MSG_ERROR,
                             STATUS_ENGINE_FAIL)
            finally:
                player_one.close()
                player_two.close()

    def test_extended_turn_failure_is_reported(self) -> None:
        with running_server(self.server, FAKE_EXTENDED_FAIL="1") as port:
            player_one, player_two = join_pair(port, 575)
            try:
                send_frame(player_one, MSG_SHIP_SUBMIT, payload=ships_payload())
                send_frame(player_two, MSG_SHIP_SUBMIT, payload=ships_payload())
                recv_frame(player_one)
                recv_frame(player_two)
                send_frame(player_one, MSG_EXT_MOVE_SUBMIT, payload=b"A1\0\0")
                assert_frame(self, recv_frame(player_one), MSG_ERROR,
                             STATUS_ENGINE_FAIL)
                assert_frame(self, recv_frame(player_two), MSG_ERROR,
                             STATUS_ENGINE_FAIL)
            finally:
                player_one.close()
                player_two.close()

    def test_join_failure_is_reported(self) -> None:
        with running_server(self.server, FAKE_GAME_INIT_FAIL="1") as port:
            sock = socket.create_connection(("127.0.0.1", port), timeout=2)
            try:
                send_frame(sock, MSG_JOIN, payload=(600).to_bytes(4, "big"))
                assert_frame(self, recv_frame(sock), MSG_ERROR, STATUS_ENGINE_FAIL)
            finally:
                sock.close()

    def test_engine_startup_failure_exits_cleanly(self) -> None:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        environment = os.environ.copy()
        environment["FAKE_ENGINE_INIT_FAIL"] = "1"
        result = subprocess.run(
            [str(self.server), str(port)], cwd=ROOT, env=environment,
            capture_output=True, text=True, check=False, timeout=2,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("Failed to initialise engine", result.stderr)

    def test_disconnect_ends_matched_game(self) -> None:
        with running_server(self.server) as port:
            first = join_client(port, 700)
            first.close()
            second = join_client(port, 700)
            try:
                assert_frame(self, recv_frame(second), MSG_GAME_READY)
                message_type, status, payload = recv_frame(second)
                self.assertEqual(message_type, MSG_ERROR)
                self.assertIn(status, (STATUS_PROTOCOL_ERROR,
                                       STATUS_DISCONNECTED))
                self.assertEqual(payload, b"")
            finally:
                second.close()

    def test_client_api_round_trip(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        errors: list[BaseException] = []

        def fake_server() -> None:
            try:
                sock, _ = listener.accept()
                with sock:
                    self.assertEqual(recv_frame(sock),
                                     (MSG_JOIN, STATUS_OK, (77).to_bytes(4, "big")))
                    send_frame(sock, MSG_JOIN_ACCEPTED)
                    send_frame(sock, MSG_GAME_READY)
                    message_type, status, payload = recv_frame(sock)
                    self.assertEqual((message_type, status),
                                     (MSG_SHIP_SUBMIT, STATUS_OK))
                    self.assertEqual(payload, ships_payload())
                    send_frame(sock, MSG_SHIP_RESULT, payload=b"\x02")
                    self.assertEqual(recv_frame(sock),
                                     (MSG_MOVE_SUBMIT, STATUS_OK, b"A1\0\0"))
                    send_frame(sock, MSG_MOVE_RESULT, payload=b"\x01")
                    send_frame(sock, MSG_OPPONENT_MOVE, payload=b"C1\0\0\x04")
                    self.assertEqual(recv_frame(sock),
                                     (MSG_EXT_MOVE_SUBMIT, STATUS_OK, b"B1\0\0"))
                    send_frame(sock, MSG_EXT_MOVE_RESULT, payload=b"\x04xyz")
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=fake_server, daemon=True)
        thread.start()
        try:
            result = subprocess.run(
                [str(self.client_probe), "127.0.0.1", str(port), "77"],
                cwd=ROOT, capture_output=True, text=True, check=False,
                timeout=5,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finally:
            listener.close()
            thread.join(timeout=2)
        self.assertFalse(errors, errors)


def main() -> int:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RepositoryTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
