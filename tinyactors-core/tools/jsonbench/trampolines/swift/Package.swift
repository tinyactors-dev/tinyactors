// swift-tools-version:6.0
// The cost per call of a Swift function behind the json datamodel's ABI
// (ta_json_function), as a Swift binding would make one. A spike; see run.sh.
import PackageDescription

let core = Context.packageDirectory + "/../../../.."

let package = Package(
    name: "JSONBench",
    dependencies: [
        .package(url: "https://github.com/valpackett/SwiftCBOR", exact: "0.6.0"),
    ],
    targets: [
        // The C side: core's headers, the driver loop (timed in C, so it
        // includes the C -> Swift crossing), and core's release static
        // libraries (ta_cbor_reader/writer are in the base layer).
        .target(
            name: "CTinyactors",
            cSettings: [
                .unsafeFlags(["-I" + core + "/include", "-O2"]),
            ],
            linkerSettings: [
                .unsafeFlags(["-L" + core + "/build/release/lib"]),
                .linkedLibrary("tinyactors_base"),
            ]
        ),
        .executableTarget(
            name: "JSONBench",
            dependencies: [
                "CTinyactors",
                .product(name: "SwiftCBOR", package: "SwiftCBOR"),
            ],
            swiftSettings: [.unsafeFlags(["-Xcc", "-I" + core + "/include", "-wmo"])]
        ),
    ],
    swiftLanguageModes: [.v5]
)
