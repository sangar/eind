// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "EindSearch",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(name: "EindSearch"),
        .testTarget(name: "EindSearchTests", dependencies: ["EindSearch"]),
    ]
)
