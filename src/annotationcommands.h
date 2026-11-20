#pragma once

#include <QStringList>

// The `annotations` command (#86): a book's highlights, margin notes,
// bookmarks and book ink, or the whole library's, listed, one added on words
// of the book, changed or removed, and exported as Markdown, for Readwise or,
// of a PDF, written into a copy of it. The MCP tools annotations_list,
// annotation_add, annotation_change, annotation_remove and annotations_export
// are this command (annotationtools.cpp).
int runAnnotationsCli(const QStringList &arguments);
