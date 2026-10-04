export default function point(x: number, y: number) {
    return { x, y };
}

// a comment

export default class A {}

export async function readFile(filename: string): Promise<Buffer>
