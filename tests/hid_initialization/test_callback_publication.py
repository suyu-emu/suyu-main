"""HID timing callbacks must not be published before their controllers exist.

Follow the real ResourceManager initializer calls and allocations in source.
Treat scheduling as an immediate callback: CoreTiming runs on another thread
and provides no grace period for completing initialization after publication.
"""
from pathlib import Path
import re
import unittest


SOURCE = Path(__file__).resolve().parents[2] / "src/hid_core/resource_manager.cpp"


def body_at(text, opening, start="{", stop="}"):
    depth = 1
    end = opening + 1
    while depth:
        if text[end] == start:
            depth += 1
        elif text[end] == stop:
            depth -= 1
        end += 1
    return text[opening + 1:end - 1]


class CallbackPublication(unittest.TestCase):
    def test_every_published_callback_has_its_controllers(self):
        text = SOURCE.read_text(encoding="utf-8")
        methods = {
            match[1]: body_at(text, match.end() - 1)
            for match in re.finditer(
                r"void ResourceManager::(Initialize\w*|Update\w+)\([^)]*\)\s*\{", text
            )
        }
        events = {}
        for event in re.finditer(
            r"(\w+_update_event)\s*=\s*Core::Timing::CreateEvent\(", text
        ):
            callback = re.search(
                r"\b(Update\w+)\(", body_at(text, event.end() - 1, "(", ")")
            )
            if callback:
                events[event[1]] = callback[1]
        self.assertEqual(set(events.values()),
                         {"UpdateNpad", "UpdateControllers", "UpdateMouseKeyboard", "UpdateMotion"})
        dependencies = {
            event: set(re.findall(r"\b(\w+)->", methods[callback]))
            for event, callback in events.items()
        }
        ready, published = set(), set()
        actions = re.compile(
            r"(?P<member>\w+)\s*=\s*std::make_shared<"
            r"|(?P<initializer>Initialize\w+)\(\);"
            r"|ScheduleLoopingEvent\([^;]*?\b(?P<event>\w+_update_event)\);", re.S,
        )

        def execute(name):
            for action in actions.finditer(methods[name]):
                member, initializer, event = (
                    action["member"], action["initializer"], action["event"]
                )
                if member:
                    ready.add(member)
                elif initializer:
                    execute(initializer)
                else:
                    self.assertIn(event, dependencies)
                    missing = dependencies[event] - ready
                    self.assertFalse(
                        missing,
                        f"{event} can run before these controllers exist: {sorted(missing)}",
                    )
                    published.add(event)

        execute("Initialize")
        self.assertEqual(published, set(events))


if __name__ == "__main__":
    unittest.main()
