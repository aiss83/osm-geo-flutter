// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "osm_geo_search",
    platforms: [
        .macOS(.v11)
    ],
    products: [
        .library(
            name: "osm_geo_search",
            type: .static,
            targets: ["osm_geo_search"]
        )
    ],
    targets: [
        .target(
            name: "osm_geo_search",
            path: "../native",
            sources: ["osm_geo_search.c"],
            publicHeadersPath: ".",
            cSettings: [
                .headerSearchPath("/opt/local/include"),
            ],
            linkerSettings: [
                .linkedLibrary("icuuc"),
                .unsafeFlags(["-L/opt/local/lib"]),
            ]
        )
    ]
)
