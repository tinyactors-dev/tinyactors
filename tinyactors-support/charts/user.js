// The user's helpers: the attachment quota, for the day a reset (a
// delayed send every 24 hours) began, and the caller as the gateway hears it.

// What is left today, with the limits of a file attached to an issue of a
// repository the user does not maintain (the issue's chart applies them;
// a maintainer's own repositories have none).
function left(used, quota) {
  var limits = guestLimits();
  return { files: quota.files - used.files, bytes: quota.bytes - used.bytes,
           maxFileBytes: limits.maxFileBytes, retentionDays: limits.retentionDays };
}

function spend(used, bytes) { return { files: used.files + 1, bytes: used.bytes + bytes }; }

function caller(githubID, login, maintainerOf, unseen, quota) {
  return { githubID: githubID, login: login, maintainerOf: maintainerOf, unseen: unseen, quota: quota };
}
