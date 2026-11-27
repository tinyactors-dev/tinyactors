// The user's helpers: the attachment quota, for the day a reset (a
// delayed send every 24 hours) began.

function left(used, quota) { return { files: quota.files - used.files, bytes: quota.bytes - used.bytes }; }

function spend(used, bytes) { return { files: used.files + 1, bytes: used.bytes + bytes }; }
