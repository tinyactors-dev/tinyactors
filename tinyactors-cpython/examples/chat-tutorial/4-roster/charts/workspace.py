# The workspace's helpers. Each takes the variables it reads.


def refusal(rooms, creating, name):
    """Why a new room is refused, or None."""
    if not isinstance(name, str) or len(name) == 0 or len(name) > 32 or not all(c in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in name):
        return "invalid name"
    if name in rooms or any(pending["name"] == name for pending in creating.values()):
        return "exists"
    return None


def without(table, key):
    return {k: v for k, v in table.items() if k != key}


def online(roster, but):
    """The users who are online, but `but`."""
    return [user for user, status in roster.items() if status != "offline" and user != but]


def came_online(roster, event):
    return roster.get(event.data["user"], "offline") == "offline" and event.data["status"] != "offline"
