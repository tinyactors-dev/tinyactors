// swift-tools-version:6.0
// Language-neutral charts, json beside staticjs, with the app's functions in
// Swift (a spike: tools/neutral): the runtime (Tinyactors), the market
// tracker (Market) and the order (Order).
import PackageDescription

let core = Context.packageDirectory + "/../../.."
let libraries = ["datamodel_json", "datamodel_staticjs", "staticjs", "host", "runtime", "compile", "system", "core", "otlp", "trace", "image", "base"]

let package = Package(
    name: "TinyactorsNeutral",
    targets: [
        .target(
            name: "CTinyactors",
            cSettings: [.unsafeFlags(["-I" + core + "/include", "-I" + core + "/../staticjs/include", "-O2"])],
            linkerSettings: [
                .unsafeFlags(["-L" + core + "/build/release/lib", "-Xlinker", "--start-group"]
                    + libraries.map { "-ltinyactors_" + $0 } + ["-Xlinker", "--end-group", "-lm"]),
            ]
        ),
        .target(name: "Tinyactors", dependencies: ["CTinyactors"],
                swiftSettings: [.unsafeFlags(["-Xcc", "-I" + core + "/include", "-Xcc", "-I" + core + "/../staticjs/include"])]),
        .executableTarget(name: "Market", dependencies: ["Tinyactors"],
                          swiftSettings: [.unsafeFlags(["-Xcc", "-I" + core + "/include", "-Xcc", "-I" + core + "/../staticjs/include"])]),
        .executableTarget(name: "Order", dependencies: ["Tinyactors"],
                          swiftSettings: [.unsafeFlags(["-Xcc", "-I" + core + "/include", "-Xcc", "-I" + core + "/../staticjs/include"])]),
    ],
    swiftLanguageModes: [.v5]
)
