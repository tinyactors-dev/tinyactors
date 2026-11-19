# The room's helpers.

def workspace_address(workspace):
    return "tinyactors://chat/@workspace." + workspace


def message(room, members, event):
    return {"room": room, "user": members[event.origin], "text": event.data["text"]}


def stored(workspace, room, members, event):
    return {"workspace": workspace, **message(room, members, event)}


def remember(history, item):
    return history[-199:] + [item]


def recent(history):
    return history[-50:]


def users(members):
    return sorted(set(members.values()))


def with_entry(table, key, value):
    return {**table, key: value}


def without_entry(table, key):
    return {k: v for k, v in table.items() if k != key}


def without_all(table, gone):
    return {k: v for k, v in table.items() if k not in gone}


def without_user(table, user):
    return {k: v for k, v in table.items() if v != user}
