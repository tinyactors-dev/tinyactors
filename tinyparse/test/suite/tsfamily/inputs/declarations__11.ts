export interface IAppState {
  public readonly users: ReadonlyArray<User>
}

export class CloningRepository {
  public readonly id = CloningRepositoryID++
}
