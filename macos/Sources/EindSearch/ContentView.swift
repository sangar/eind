import AppKit
import QuickLook
import SwiftUI

struct ContentView: View {
    @State private var model = SearchModel()
    @State private var selection: SearchResult.ID?
    @State private var previewURL: URL?
    @FocusState private var searchFieldFocused: Bool
    @FocusState private var tableFocused: Bool

    private var selectedResult: SearchResult? {
        model.results.first { $0.id == selection }
    }

    var body: some View {
        VStack(spacing: 0) {
            searchField
            Divider()
            resultsTable
            Divider()
            statusBar
        }
        .frame(minWidth: 700, minHeight: 400)
        .onAppear { searchFieldFocused = true }
    }

    private var searchField: some View {
        HStack {
            Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
            TextField("Search names, e.g. report ext:pdf dm:thisweek", text: $model.query)
                .textFieldStyle(.plain)
                .font(.title3)
                .focused($searchFieldFocused)
                .onChange(of: model.query) { model.search() }
                .onSubmit { openSelectionOrFirstResult() }
                .onKeyPress(.downArrow) { moveFocusToResults() }
        }
        .padding(12)
    }

    private var resultsTable: some View {
        Table(model.results, selection: $selection) {
            TableColumn("Name") { result in
                Label {
                    Text(result.name)
                } icon: {
                    Image(nsImage: NSWorkspace.shared.icon(forFile: result.path))
                        .resizable()
                        .frame(width: 16, height: 16)
                }
            }
            .width(min: 150, ideal: 260)
            TableColumn("Location") { result in
                Text(result.parentPath).foregroundStyle(.secondary)
            }
            TableColumn("Size") { result in
                Text(result.isDirectory ? "" : ByteCountFormatter.string(fromByteCount: result.size, countStyle: .file))
                    .monospacedDigit()
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
            .width(min: 70, ideal: 90)
            TableColumn("Modified") { result in
                Text(result.modifiedDate.map { $0.formatted(date: .numeric, time: .shortened) } ?? result.modified)
                    .monospacedDigit()
            }
            .width(min: 120, ideal: 150)
        }
        .focused($tableFocused)
        .onKeyPress(.space) { togglePreview() }
        .quickLookPreview($previewURL, in: model.results.map(\.url))
        .onChange(of: selection) {
            if previewURL != nil {
                previewURL = selectedResult?.url
            }
        }
        .contextMenu(forSelectionType: SearchResult.ID.self) { paths in
            if let result = model.results.first(where: { paths.contains($0.id) }) {
                Button("Open") { model.open(result) }
                Button("Quick Look") { previewURL = result.url }
                Button("Reveal in Finder") { model.revealInFinder(result) }
                Button("Copy Path") { copyToPasteboard(result.path) }
            }
        } primaryAction: { paths in
            if let result = model.results.first(where: { paths.contains($0.id) }) {
                model.open(result)
            }
        }
        .overlay { emptyState }
    }

    @ViewBuilder
    private var emptyState: some View {
        if let error = model.connectionError {
            ContentUnavailableView(
                "Not connected",
                systemImage: "bolt.slash",
                description: Text(error)
            )
        } else if let error = model.queryError {
            ContentUnavailableView(
                "Invalid query",
                systemImage: "exclamationmark.triangle",
                description: Text(error)
            )
        } else if model.results.isEmpty, !model.query.isEmpty {
            ContentUnavailableView.search(text: model.query)
        }
    }

    private var statusBar: some View {
        HStack {
            if model.isConnected, !model.query.isEmpty, model.queryError == nil {
                Text("\(model.total.formatted()) results in \(model.elapsedMs.formatted(.number.precision(.fractionLength(1)))) ms")
                if model.total > model.results.count {
                    Text("showing first \(model.results.count)").foregroundStyle(.secondary)
                }
            }
            Spacer()
            if let status = model.indexStatus {
                Text("\(status.files.formatted()) files, \(status.folders.formatted()) folders under \(status.roots.joined(separator: ", "))")
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .font(.callout)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    private func openSelectionOrFirstResult() {
        if let result = selectedResult ?? model.results.first {
            model.open(result)
        }
    }

    private func moveFocusToResults() -> KeyPress.Result {
        guard let first = model.results.first else { return .ignored }
        if selectedResult == nil {
            selection = first.id
        }
        tableFocused = true
        return .handled
    }

    private func togglePreview() -> KeyPress.Result {
        guard let result = selectedResult else { return .ignored }
        previewURL = previewURL == nil ? result.url : nil
        return .handled
    }

    private func copyToPasteboard(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }
}
