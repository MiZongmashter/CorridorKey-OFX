import json
import unittest

from sidecar.corridorkey_sidecar.protocol import (
    ERROR_INVALID_REQUEST,
    ERROR_MALFORMED_JSON,
    ERROR_UNKNOWN_COMMAND,
    encode_response,
    handle_line,
    make_response,
)


class ProtocolTests(unittest.TestCase):
    def test_health_response_uses_required_schema(self):
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-health",
                    "command": "health",
                    "payload": {},
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertEqual(response["request_id"], "req-health")
        self.assertIs(response["ok"], True)
        self.assertIsNone(response["error"])
        self.assertEqual(response["payload"]["backend"], "stub")
        self.assertEqual(response["payload"]["model"], "not_loaded")
        self.assertEqual(response["payload"]["warmup"], "cold")
        self.assertEqual(response["payload"]["cache"], "disabled")
        self.assertEqual(response["payload"]["gpu"], "unknown")
        self.assertEqual(response["payload"]["vram"], "unknown")

    def test_malformed_json_returns_protocol_error(self):
        response, should_shutdown = handle_line("{not valid json")

        self.assertFalse(should_shutdown)
        self.assertIsNone(response["request_id"])
        self.assertIs(response["ok"], False)
        self.assertEqual(response["payload"], {})
        self.assertEqual(response["error"]["code"], ERROR_MALFORMED_JSON)

    def test_non_finite_json_constant_is_malformed_json(self):
        response, should_shutdown = handle_line(
            '{"request_id":"req-nan","command":"health","payload":{"value":NaN}}'
        )

        self.assertFalse(should_shutdown)
        self.assertIsNone(response["request_id"])
        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], ERROR_MALFORMED_JSON)

    def test_structured_request_id_is_rejected_without_echoing_it(self):
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": {"projectName": "SecretShow"},
                    "command": "health",
                    "payload": {},
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertIsNone(response["request_id"])
        self.assertIs(response["ok"], False)
        self.assertEqual(response["payload"], {})
        self.assertEqual(response["error"]["code"], ERROR_INVALID_REQUEST)

    def test_project_like_request_id_is_rejected_without_echoing_it(self):
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "SecretShow",
                    "command": "health",
                    "payload": {},
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertIsNone(response["request_id"])
        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], ERROR_INVALID_REQUEST)

    def test_response_encoder_rejects_non_finite_values(self):
        with self.assertRaises(ValueError):
            encode_response(make_response("req-nan", True, {"value": float("nan")}, None))

    def test_unknown_command_preserves_request_id(self):
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-unknown",
                    "command": "load_model",
                    "payload": {},
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertEqual(response["request_id"], "req-unknown")
        self.assertIs(response["ok"], False)
        self.assertEqual(response["payload"], {})
        self.assertEqual(response["error"]["code"], ERROR_UNKNOWN_COMMAND)

    def test_shutdown_sets_shutdown_flag(self):
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-shutdown",
                    "command": "shutdown",
                    "payload": {},
                }
            )
        )

        self.assertTrue(should_shutdown)
        self.assertEqual(response["request_id"], "req-shutdown")
        self.assertIs(response["ok"], True)
        self.assertEqual(response["payload"], {"shutdown": "accepted"})
        self.assertIsNone(response["error"])


if __name__ == "__main__":
    unittest.main()
