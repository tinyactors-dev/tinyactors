// swift-tools-version:6.0
// The precompiled datamodel in Swift (a spike: tools/precompile): the
// runtime (Tinyactors), the generator and its build plugin, and the order
// example, whose order.scxml the plugin turns into Swift at build time.
import PackageDescription

let core = Context.packageDirectory + "/../../.."
let libraries = ["datamodel_precompiled", "host", "runtime", "compile", "system", "core", "otlp", "trace", "image", "base"]

let package = Package(
    name: "TinyactorsPrecompiled",
    targets: [
        .target(
            name: "CTinyactors",
            cSettings: [.unsafeFlags(["-I" + core + "/include", "-O2"])],
            linkerSettings: [
                .unsafeFlags(["-L" + core + "/build/release/lib", "-Xlinker", "--start-group"]
                    + libraries.map { "-ltinyactors_" + $0 } + ["-Xlinker", "--end-group", "-lm"]),
            ]
        ),
        .target(name: "Tinyactors", dependencies: ["CTinyactors"],
                swiftSettings: [.unsafeFlags(["-Xcc", "-I" + core + "/include"])]),
        .executableTarget(name: "tinyactors-gen"),
        .plugin(name: "TinyactorsPlugin", capability: .buildTool(), dependencies: ["tinyactors-gen"]),
        .executableTarget(name: "Order", dependencies: ["Tinyactors"],
                          swiftSettings: [.unsafeFlags(["-Xcc", "-I" + core + "/include"])],
                          plugins: ["TinyactorsPlugin"]),
    ],
    swiftLanguageModes: [.v5]
)
