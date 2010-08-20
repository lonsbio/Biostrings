/****************************************************************************
 *                       Read/write FASTA/FASTQ files                       *
 *                           Author: Herve Pages                            *
 ****************************************************************************/
#include "Biostrings.h"
#include "IRanges_interface.h"

static int debug = 0;

SEXP debug_XStringSet_io()
{
#ifdef DEBUG_BIOSTRINGS
	debug = !debug;
	Rprintf("Debug mode turned %s in file %s\n",
		debug ? "on" : "off", __FILE__);
#else
	Rprintf("Debug mode not available in file %s\n", __FILE__);
#endif
	return R_NilValue;
}

#define LINEBUF_SIZE 20001
static char errmsg_buf[200];


/****************************************************************************
 * Read FASTA.
 */

typedef struct fasta_loader {
	int nrec;
	void (*add_desc)(const cachedCharSeq *dataline);
	void (*add_empty_seq)(int nrec);
	void (*append_to_last_seq)(const cachedCharSeq *dataline);
} FASTAloader;

/*
 * Even if the standard FASTA markup is made of 1-letter sympols, we want to
 * be able to eventually support longer sympols.
 */
static const char *FASTA_comment_markup = ";", *FASTA_desc_markup = ">";

/*
 * The LENGTHONLY storage handlers only store the lengths of the sequences.
 */

static IntAE seqlengths_buf;

static void add_empty_seq_LENGTHONLY(int nrec)
{
	IntAE_insert_at(&seqlengths_buf, seqlengths_buf.nelt, 0);
	return;
}

static void append_to_last_seq_LENGTHONLY(const cachedCharSeq *dataline)
{
	seqlengths_buf.elts[seqlengths_buf.nelt - 1] += dataline->length;
	return;
}


/*
 * The CHARAEAE storage handlers use 2 Auto-Extending buffers to keep all the
 * data coming from the FASTA file in a single pass. The downside is that
 * the content of the 2 Auto-Extending buffers must then be turned into an
 * SEXP before it can be returned to the user.
 */

static CharAEAE descs_buf;

static void add_desc_CHARAEAE(const cachedCharSeq *dataline)
{
	// This works only because dataline->seq is nul-terminated!
	append_string_to_CharAEAE(&descs_buf, dataline->seq);
	return;
}


/*
 * The FASTA_seqbuf storage handlers.
 */

static cachedXVectorList FASTA_seqbuf;
static cachedCharSeq FASTA_seqbuf_elt;
static const int *FASTA_lkup;
static int FASTA_lkup_length;

static void add_empty_seq_to_FASTA_seqbuf(int nrec)
{
	FASTA_seqbuf_elt = get_cachedXRawList_elt(&FASTA_seqbuf, nrec);
	FASTA_seqbuf_elt.length = 0;
	return;
}

static void append_to_last_seq_in_FASTA_seqbuf(const cachedCharSeq *dataline)
{
	int i1;

	i1 = FASTA_seqbuf_elt.length;
	FASTA_seqbuf_elt.length += dataline->length;
	/* FASTA_seqbuf_elt.seq is a const char * so we need to cast it to
           char * before we can write to it */
	Ocopy_bytes_to_i1i2_with_lkup(i1, FASTA_seqbuf_elt.length - 1,
		(char *) FASTA_seqbuf_elt.seq, FASTA_seqbuf_elt.length,
		dataline->seq, dataline->length,
		FASTA_lkup, FASTA_lkup_length);
	return;
}


/*
 * Ignore empty lines and lines starting with 'FASTA_comment_markup' like in
 * the original Pearson FASTA format.
 */
static const char *parse_FASTA_file(FILE *stream, int *recno,
		int nrec, int skip, FASTAloader *loader)
{
	int FASTA_comment_markup_length, FASTA_desc_markup_length,
	    lineno, load_record, new_record;
	char linebuf[LINEBUF_SIZE];
	cachedCharSeq dataline;

	FASTA_comment_markup_length = strlen(FASTA_comment_markup);
	FASTA_desc_markup_length = strlen(FASTA_desc_markup);
	lineno = 0;
	load_record = -1;
	while (fgets(linebuf, LINEBUF_SIZE, stream) != NULL) {
		lineno++;
		dataline.length = rtrimline(linebuf, -1);
		// dataline.length > LINEBUF_SIZE - 1 should never happen
		if (dataline.length >= LINEBUF_SIZE - 1) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "cannot read line %d, line is too long",
				 lineno);
			return errmsg_buf;
		}
		if (dataline.length == 0)
			continue; // we ignore empty lines
		if (strncmp(linebuf, FASTA_comment_markup,
				FASTA_comment_markup_length) == 0)
			continue; // we ignore comment lines
		dataline.seq = linebuf;
		new_record = strncmp(linebuf, FASTA_desc_markup,
				FASTA_desc_markup_length) == 0;
		if (new_record) {
			load_record = loader != NULL && *recno >= skip;
			if (load_record && nrec >= 0)
				load_record = *recno < skip + nrec;
			if (load_record && loader->add_desc != NULL) {
				dataline.seq += FASTA_desc_markup_length;
				dataline.length -= FASTA_desc_markup_length;
				loader->add_desc(&dataline);
			}
			if (load_record && loader->add_empty_seq != NULL)
				loader->add_empty_seq(loader->nrec);
			if (load_record)
				loader->nrec++;
			(*recno)++;
			continue;
		}
		if (load_record == -1) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "\"%s\" expected at beginning of line %d",
				 FASTA_desc_markup, lineno);
			return errmsg_buf;
		}
		if (load_record && loader->append_to_last_seq != NULL)
			loader->append_to_last_seq(&dataline);
	}
	return NULL;
}

/* --- .Call ENTRY POINT --- */
SEXP fasta_info(SEXP efp_list, SEXP nrec, SEXP skip, SEXP use_descs)
{
	int nrec0, skip0, i, recno;
	FASTAloader loader;
	FILE *stream;
	SEXP ans, ans_names;
	const char *errmsg;

	nrec0 = INTEGER(nrec)[0];
	skip0 = INTEGER(skip)[0];
	seqlengths_buf = new_IntAE(0, 0, 0);
	loader.nrec = 0;
	if (LOGICAL(use_descs)[0]) {
		loader.add_desc = &add_desc_CHARAEAE;
		descs_buf = new_CharAEAE(0, 0);
	} else {
		loader.add_desc = NULL;
	}
	loader.add_empty_seq = &add_empty_seq_LENGTHONLY;
	loader.append_to_last_seq = &append_to_last_seq_LENGTHONLY;
	recno = 0;
	for (i = 0; i < LENGTH(efp_list); i++) {
		stream = R_ExternalPtrAddr(VECTOR_ELT(efp_list, i));
		errmsg = parse_FASTA_file(stream, &recno,
				nrec0, skip0, &loader);
		if (errmsg != NULL)
			error("reading FASTA file %s: %s",
			      STRING_ELT(GET_NAMES(efp_list), i), errmsg_buf);
	}
	PROTECT(ans = new_INTEGER_from_IntAE(&seqlengths_buf));
	if (LOGICAL(use_descs)[0]) {
		PROTECT(ans_names = new_CHARACTER_from_CharAEAE(&descs_buf));
		SET_NAMES(ans, ans_names);
		UNPROTECT(1);
	}
	UNPROTECT(1);
	return ans;
}

/* --- .Call ENTRY POINT --- */
SEXP read_fasta_in_XStringSet(SEXP efp_list, SEXP nrec, SEXP skip,
		SEXP set_names, SEXP elementType, SEXP lkup)
{
	int nrec0, skip0, i, recno;
	SEXP ans_width, ans_names, ans;
	const char *element_type;
	char classname[40]; // longest string will be "DNAStringSet"
	FASTAloader loader;
	FILE *stream;

	nrec0 = INTEGER(nrec)[0];
	skip0 = INTEGER(skip)[0];
	PROTECT(ans_width = fasta_info(efp_list, nrec, skip, set_names));
	PROTECT(ans_names = GET_NAMES(ans_width));
	SET_NAMES(ans_width, R_NilValue);
	element_type = CHAR(STRING_ELT(elementType, 0));
	if (snprintf(classname, sizeof(classname), "%sSet", element_type)
	    >= sizeof(classname))
	{
		UNPROTECT(2);
		error("Biostrings internal error in "
		      "read_fasta_in_XStringSet(): 'elementType' too long");
	}
	PROTECT(ans = alloc_XRawList(classname, element_type, ans_width));
	_set_XStringSet_names(ans, ans_names);
	FASTA_seqbuf = cache_XVectorList(ans);
	if (lkup == R_NilValue) {
		FASTA_lkup = NULL;
	} else {
		FASTA_lkup = INTEGER(lkup);
		FASTA_lkup_length = LENGTH(lkup);
	}
	loader.nrec = 0;
	loader.add_desc = NULL;
	loader.add_empty_seq = &add_empty_seq_to_FASTA_seqbuf;
	loader.append_to_last_seq = &append_to_last_seq_in_FASTA_seqbuf;
	recno = 0;
	for (i = 0; i < LENGTH(efp_list); i++) {
		stream = R_ExternalPtrAddr(VECTOR_ELT(efp_list, i));
		rewind(stream);
		parse_FASTA_file(stream, &recno, nrec0, skip0, &loader);
	}
	UNPROTECT(3);
	return ans;
}

/* --- .Call ENTRY POINT --- */
SEXP write_XStringSet_to_fasta(SEXP x, SEXP efp_list, SEXP width, SEXP lkup)
{
	cachedXStringSet X;
	int x_length, width0, lkup_length, i, j1, j2, dest_nbytes;
	FILE *stream;
	const int *lkup0;
	SEXP x_names, desc;
	cachedCharSeq X_elt;
	char linebuf[LINEBUF_SIZE];

	X = _cache_XStringSet(x);
	x_length = _get_cachedXStringSet_length(&X);
	stream = R_ExternalPtrAddr(VECTOR_ELT(efp_list, 0));
	width0 = INTEGER(width)[0];
	if (width0 >= LINEBUF_SIZE)
		error("'width' must be <= %d", LINEBUF_SIZE - 1);
	linebuf[width0] = 0;
	if (lkup == R_NilValue) {
		lkup0 = NULL;
		lkup_length = 0;
	} else {
		lkup0 = INTEGER(lkup);
		lkup_length = LENGTH(lkup);
	}
	x_names = get_XVectorList_names(x);
	for (i = 0; i < x_length; i++) {
		if (fputs(FASTA_desc_markup, stream) == EOF)
			error("write error");
		if (x_names != R_NilValue) {
			desc = STRING_ELT(x_names, i);
			if (desc == NA_STRING)
				error("'names(x)' contains NAs");
			if (fputs(CHAR(desc), stream) == EOF)
				error("write error");
		}
		if (fputs("\n", stream) == EOF)
			error("write error");
		X_elt = _get_cachedXStringSet_elt(&X, i);
		for (j1 = 0; j1 < X_elt.length; j1 += width0) {
			j2 = j1 + width0;
			if (j2 > X_elt.length)
				j2 = X_elt.length;
			dest_nbytes = j2 - j1;
			j2--;
			Ocopy_bytes_from_i1i2_with_lkup(j1, j2,
				linebuf, dest_nbytes,
				X_elt.seq, X_elt.length,
				lkup0, lkup_length);
			linebuf[dest_nbytes] = 0;
			if (fputs(linebuf, stream) == EOF)
				error("write error");
			if (fputs("\n", stream) == EOF)
				error("write error");
		}
	}
	return R_NilValue;
}


/****************************************************************************
 * Read FASTQ.
 */

typedef struct fastq_loader {
	int nrec;
	void (*add_seqid)(const cachedCharSeq *dataline);
	void (*add_seq)(int nrec, const cachedCharSeq *dataline);
	void (*add_qualid)(const cachedCharSeq *dataline);
	void (*add_qual)(const cachedCharSeq *dataline);
} FASTQloader;

static const char *FASTQ_line1_markup = "@", *FASTQ_line3_markup = "+";

/*
 * The WIDTHONLY storage handlers keep only the common width of the sequences
 * (NA if the set of sequences is not rectangular).
 */

static int FASTQ_width;

static void add_seq_WIDTHONLY(int recno, const cachedCharSeq *dataline)
{
	if (recno == 0) {
		FASTQ_width = dataline->length;
		return;
	}
	if (FASTQ_width == NA_INTEGER || dataline->length == FASTQ_width)
		return;
	FASTQ_width = NA_INTEGER;
	return;
}


/*
 * The FASTQ_seqbuf storage handlers.
 */

static cachedXVectorList FASTQ_seqbuf;
static const int *FASTQ_lkup;
static int FASTQ_lkup_length;

static void append_seq_to_FASTQ_seqbuf(int recno, const cachedCharSeq *dataline)
{
	cachedCharSeq FASTQ_seqbuf_elt;

	FASTQ_seqbuf_elt = get_cachedXRawList_elt(&FASTQ_seqbuf, recno);
	/* FASTQ_seqbuf_elt.seq is a const char * so we need to cast it to
           char * before we can write to it */
	Ocopy_bytes_to_i1i2_with_lkup(0, FASTQ_seqbuf_elt.length - 1,
		(char *) FASTQ_seqbuf_elt.seq, FASTQ_seqbuf_elt.length,
		dataline->seq, dataline->length,
		FASTQ_lkup, FASTQ_lkup_length);
	return;
}


/*
 * Ignore empty lines.
 */
static const char *parse_FASTQ_file(FILE *stream, int *recno,
		int nrec, int skip, FASTQloader *loader)
{
	int FASTQ_line1_markup_length, FASTQ_line3_markup_length,
	    lineno, lineinrecno, load_record;
	char linebuf[LINEBUF_SIZE];
	cachedCharSeq dataline;

	FASTQ_line1_markup_length = strlen(FASTQ_line1_markup);
	FASTQ_line3_markup_length = strlen(FASTQ_line3_markup);
	lineno = lineinrecno = 0;
	while (fgets(linebuf, LINEBUF_SIZE, stream) != NULL) {
		lineno++;
		dataline.length = rtrimline(linebuf, -1);
		// dataline.length > LINEBUF_SIZE - 1 should never happen
		if (dataline.length >= LINEBUF_SIZE - 1) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "cannot read line %d, line is too long",
				 lineno);
			return errmsg_buf;
		}
		if (dataline.length == 0)
			continue; // we ignore empty lines
		dataline.seq = linebuf;
		lineinrecno++;
		if (lineinrecno > 4)
			lineinrecno = 1;
		switch (lineinrecno) {
		    case 1:
			if (strncmp(linebuf, FASTQ_line1_markup,
					FASTQ_line1_markup_length) != 0) {
				snprintf(errmsg_buf, sizeof(errmsg_buf),
					 "\"%s\" expected at beginning of line %d",
					 FASTQ_line1_markup, lineno);
				return errmsg_buf;
			}
			load_record = loader != NULL && *recno >= skip;
			if (load_record && nrec >= 0)
				load_record = *recno < skip + nrec;
			if (load_record && loader->add_seqid != NULL) {
				dataline.seq += FASTQ_line1_markup_length;
				dataline.length -= FASTQ_line1_markup_length;
				loader->add_seqid(&dataline);
			}
		    break;
		    case 2:
			if (load_record && loader->add_seq != NULL)
				loader->add_seq(loader->nrec, &dataline);
		    break;
		    case 3:
			if (strncmp(linebuf, FASTQ_line3_markup,
					FASTQ_line3_markup_length) != 0) {
				snprintf(errmsg_buf, sizeof(errmsg_buf),
					 "\"%s\" expected at beginning of line %d",
					 FASTQ_line3_markup, lineno);
				return errmsg_buf;
			}
			if (load_record && loader->add_qualid != NULL) {
				dataline.seq += FASTQ_line3_markup_length;
				dataline.length -= FASTQ_line3_markup_length;
				loader->add_qualid(&dataline);
			}
		    break;
		    case 4:
			if (load_record && loader->add_qual != NULL)
				loader->add_qual(&dataline);
			if (load_record)
				loader->nrec++;
			(*recno)++;
		    break;
		}
	}
	return NULL;
}

/* --- .Call ENTRY POINT --- */
SEXP fastq_geometry(SEXP efp_list, SEXP nrec, SEXP skip)
{
	int nrec0, skip0, i, recno;
	FASTQloader loader;
	FILE *stream;
	const char *errmsg;
	SEXP ans;

	nrec0 = INTEGER(nrec)[0];
	skip0 = INTEGER(skip)[0];
	FASTQ_width = NA_INTEGER;
	loader.nrec = 0;
	loader.add_seqid = NULL;
	loader.add_seq = add_seq_WIDTHONLY;
	loader.add_qualid = NULL;
	loader.add_qual = NULL;
	recno = 0;
	for (i = 0; i < LENGTH(efp_list); i++) {
		stream = R_ExternalPtrAddr(VECTOR_ELT(efp_list, i));
		errmsg = parse_FASTQ_file(stream, &recno,
				nrec0, skip0, &loader);
		if (errmsg != NULL)
			error("reading FASTQ file %s: %s",
			      STRING_ELT(GET_NAMES(efp_list), i), errmsg_buf);
	}
	PROTECT(ans = NEW_INTEGER(2));
	INTEGER(ans)[0] = loader.nrec;
	INTEGER(ans)[1] = FASTQ_width;
	UNPROTECT(1);
	return ans;
}

/* --- .Call ENTRY POINT ---
 * 'set_names' is ignored.
 */
SEXP read_fastq_in_XStringSet(SEXP efp_list, SEXP nrec, SEXP skip,
		SEXP set_names, SEXP elementType, SEXP lkup)
{
	int nrec0, skip0, ans_length, i, recno;
	SEXP ans, ans_geom, ans_width;
	const char *element_type;
	char classname[40]; // longest string will be "DNAStringSet"
	FASTQloader loader;
	FILE *stream;

	nrec0 = INTEGER(nrec)[0];
	skip0 = INTEGER(skip)[0];
	PROTECT(ans_geom = fastq_geometry(efp_list, nrec, skip));
	ans_length = INTEGER(ans_geom)[0];
	PROTECT(ans_width = NEW_INTEGER(ans_length));
	if (ans_length != 0) {
		if (INTEGER(ans_geom)[1] == NA_INTEGER) {
			UNPROTECT(2);
			error("read_fastq_in_XStringSet(): FASTQ files with "
			      "variable sequence lengths are not supported yet");
		}
		for (recno = 0; recno < ans_length; recno++)
			INTEGER(ans_width)[recno] = INTEGER(ans_geom)[1];
	}
	element_type = CHAR(STRING_ELT(elementType, 0));
	if (snprintf(classname, sizeof(classname), "%sSet", element_type)
	    >= sizeof(classname))
	{
		UNPROTECT(2);
		error("Biostrings internal error in "
		      "read_fasta_in_XStringSet(): 'elementType' too long");
	}
	PROTECT(ans = alloc_XRawList(classname, element_type, ans_width));
	FASTQ_seqbuf = cache_XVectorList(ans);
	if (lkup == R_NilValue) {
		FASTQ_lkup = NULL;
	} else {
		FASTQ_lkup = INTEGER(lkup);
		FASTQ_lkup_length = LENGTH(lkup);
	}
	loader.nrec = 0;
	loader.add_seqid = NULL;
	loader.add_seq = append_seq_to_FASTQ_seqbuf;
	loader.add_qualid = NULL;
	loader.add_qual = NULL;
	recno = 0;
	for (i = 0; i < LENGTH(efp_list); i++) {
		stream = R_ExternalPtrAddr(VECTOR_ELT(efp_list, i));
		rewind(stream);
		parse_FASTQ_file(stream, &recno, nrec0, skip0, &loader);
	}
	UNPROTECT(3);
	return ans;
}

