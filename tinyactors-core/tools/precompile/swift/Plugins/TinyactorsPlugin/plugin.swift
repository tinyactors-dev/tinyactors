// Runs tinyactors-gen on every .scxml file of a target, so a chart is a
// source file like any other: its Swift is generated and compiled with the
// target, and the compiler's errors name the chart's lines.
import Foundation
import PackagePlugin

@main
struct TinyactorsPlugin: BuildToolPlugin {
    func createBuildCommands(context: PluginContext, target: Target) throws -> [Command] {
        guard let target = target as? SourceModuleTarget else { return [] }
        let generator = try context.tool(named: "tinyactors-gen")
        // core's tool, built by `mise run build --release` (tools/precompile)
        let table = context.package.directoryURL.appending(path: "../../../build/release/bin/ta-precompile-table").standardized
        return target.sourceFiles(withSuffix: "scxml").map { file in
            let output = context.pluginWorkDirectoryURL.appending(path: file.url.deletingPathExtension().lastPathComponent + ".generated.swift")
            return .buildCommand(displayName: "tinyactors-gen \(file.url.lastPathComponent)", executable: generator.url,
                                 arguments: [table.path, file.url.path, output.path],
                                 inputFiles: [file.url, table], outputFiles: [output])
        }
    }
}
