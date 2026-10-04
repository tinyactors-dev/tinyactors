type FileMergeData = {
  document: Doc,
  headDocument: Doc,
  baseDocument: Doc,
  conflicts: ConflictInfo[]
}

type ConflictInfo = {
  start?: LineHandle,
  middle?: LineHandle,
  end?: LineHandle
}
