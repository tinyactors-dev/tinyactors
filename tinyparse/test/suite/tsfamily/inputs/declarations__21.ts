class Foo {
  public async waitFor<T>(func: () => T | Promise<T | undefined>, accept?: (result: T) => boolean | Promise<boolean>, timeoutMessage?: string, retryCount?: number): Promise<T>;

  public readonly async bar?<T>();
  private static bar();
  private static async bar(): T;
}
