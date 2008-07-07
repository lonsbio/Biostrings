### =========================================================================
### AlignedXStringSet objects
### -------------------------------------------------------------------------
### An AlignedXStringSet object contains an alignment.


setClass("AlignedXStringSet",
    representation(
        unaligned="XStringSet",
        quality="XStringSet",
        range="IRanges",
        mismatch = "list",
        indel="list"
    )
)


### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Initialization.
###

setMethod("initialize", "AlignedXStringSet",
    function(.Object, unaligned, quality, range, mismatch, indel, check = TRUE)
    {
        slot(.Object, "unaligned", check = check) <- unaligned
        slot(.Object, "quality", check = check) <- quality
        slot(.Object, "range", check = check) <- range
        slot(.Object, "mismatch", check = check) <- mismatch
        slot(.Object, "indel", check = check) <- indel
        .Object
    }
)


### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Validity.
###

.valid.AlignedXStringSet <- function(object)
{
    message <- character(0)
    if (length(object@range) != length(mismatch(object)))
        message <- c(message, "length(range) != length(mismatch)")
    if (length(mismatch(object)) != length(indel(object)))
        message <- c(message, "length(mismatch) != length(indel)")
    if (!(length(object@unaligned) %in% c(1, length(object@range))))
        message <- c(message, "length(unaligned) != 1 or length(range)")
    if (!(length(object@quality) %in% c(1, length(object@range))))
        message <- c(message, "length(quality) != 1 or length(range)")
    if (length(message) == 0)
        message <- NULL
    message
}

setValidity("AlignedXStringSet",
    function(object)
    {
        problems <- .valid.AlignedXStringSet(object)
        if (is.null(problems)) TRUE else problems
    }
)


### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Accessor methods.
###

setGeneric("unaligned", function(x) standardGeneric("unaligned"))
setMethod("unaligned", "AlignedXStringSet", function(x) x@unaligned)

setGeneric("aligned", function(x) standardGeneric("aligned"))
setMethod("aligned", "AlignedXStringSet",
          function(x) {
              codecX <- codec(x)
              if (is.null(codecX)) {
                  gapCode <- charToRaw("-")
              } else {
                  letters2codes <- codecX@codes
                  names(letters2codes) <- codecX@letters
                  gapCode <- as.raw(letters2codes[["-"]])
              }
              .Call("AlignedXStringSet_align_aligned", x, gapCode, PACKAGE="Biostrings")
          })

setMethod("start", "AlignedXStringSet", function(x) start(x@range))
setMethod("end", "AlignedXStringSet", function(x) end(x@range))
setMethod("width", "AlignedXStringSet", function(x) width(x@range))
setMethod("mismatch", c(pattern = "AlignedXStringSet", x = "missing"),
          function(pattern, x, fixed) pattern@mismatch)
setMethod("nmismatch", c(pattern = "AlignedXStringSet", x = "missing"),
          function(pattern, x, fixed) {
              mismatches <- mismatch(pattern)
              .Call("Biostrings_length_vectors_in_list", mismatches, PACKAGE="Biostrings")
          })
setGeneric("indel", function(x) standardGeneric("indel"))
setMethod("indel", "AlignedXStringSet", function(x) x@indel)
setMethod("length", "AlignedXStringSet", function(x) length(x@range))
setMethod("nchar", "AlignedXStringSet",
          function(x, type="chars", allowNA=FALSE) .Call("AlignedXStringSet_nchar", x, PACKAGE="Biostrings"))
setMethod("alphabet", "AlignedXStringSet", function(x) alphabet(unaligned(x)))
setMethod("codec", "AlignedXStringSet", function(x) codec(unaligned(x)))


### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### The "show" method.
###

### TODO: Make the "show" method to format the alignment in a SGD fashion
### i.e. split in 60-letter blocks and use the "|" character to highlight
### exact matches.
setMethod("show", "AlignedXStringSet",
    function(object)
    {
        if (width(object)[1] == 0)
          cat("[1] \"\"\n")
        else
          cat(paste("[", start(object)[1], "]", sep = ""),
              toSeqSnippet(aligned(object)[[1]], getOption("width") - 8), "\n")
    }
)


### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### The "as.character" method.
###

setMethod("as.character", "AlignedXStringSet",
    function(x)
    {
        as.character(aligned(x))
    }
)

setMethod("toString", "AlignedXStringSet", function(x, ...) as.character(x))


### - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
### Subsetting.
###

.safe.subset.XStringSet <- function(x, i)
{
    if (length(x) == 1) x else x[i]
}

setMethod("[", "AlignedXStringSet",
    function(x, i, j, ..., drop)
    {
        if (!missing(j) || length(list(...)) > 0)
            stop("invalid subsetting")
        if (missing(i) || (is.logical(i) && all(i)))
            return(x)
        if (is.logical(i))
            i <- which(i)
        if (!is.numeric(i) || any(is.na(i)))
            stop("invalid subsetting")
        if (any(i < 1) || any(i > length(x)))
            stop("subscript out of bounds")
        new("AlignedXStringSet",
            unaligned = .safe.subset.XStringSet(x@unaligned, i),
            quality = .safe.subset.XStringSet(x@quality, i),
            range = x@range[i,,drop = FALSE],
            mismatch = x@mismatch[i], indel = x@indel[i])
    }
)

setReplaceMethod("[", "AlignedXStringSet",
    function(x, i, j,..., value)
    {
        stop("attempt to modify the value of a ", class(x), " instance")
    }
)

setMethod("rep", "AlignedXStringSet",
    function(x, times)
    {
        x[rep.int(1:length(x), times)]
    }
)