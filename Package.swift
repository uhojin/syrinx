// swift-tools-version:5.10
import PackageDescription

let package = Package(
    name: "Syrinx",
    platforms: [
        .macOS(.v14)
    ],
    dependencies: [
        .package(url: "https://github.com/soffes/HotKey.git", from: "0.2.1")
    ],
    targets: [
        .executableTarget(
            name: "Syrinx",
            dependencies: [
                .product(name: "HotKey", package: "HotKey")
            ],
            linkerSettings: [
                .linkedFramework("CoreAudio")
            ]
        )
    ]
)
