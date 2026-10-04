class A extends B {
    constructor(x: number, y: number) {
        super(x);
    }
    public toString() {
        return super.toString() + " y=" + this.y;
    }
}
