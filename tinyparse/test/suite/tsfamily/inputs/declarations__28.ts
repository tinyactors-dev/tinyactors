abstract class Foo {
}

abstract class Animal {
    readonly abstract prop: string;
    requiredProp!: string;
    abstract makeSound(): void;
    abstract get readonlyProp(): string;
    protected abstract readonlyProp?(): string;
    move(): void {
        console.log("roaming the earth...");
    }
}

@bar
abstract class Foo {
}
