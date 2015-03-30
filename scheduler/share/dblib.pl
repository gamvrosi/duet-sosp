#!/usr/bin/perl

#
# dblib.pl
# Copyright (C) 1991-2002 by John Heidemann <johnh@isi.edu>
# $Id: dblib.pl,v 1.41 2003/05/23 04:15:47 johnh Exp $
#
# This program is distributed under terms of the GNU general
# public license, version 2.  See the file COPYING
# in $dblibdir for details.
#

$col_headertag = "#h";
$list_headertag = "#L";
$headertag_regexp = "#[hL]";

$fs_code = 'D';
$header_fsre = "[ \t\n]+";
$fsre = "[ \t\n]+";
$outfs = "\t";
$header_outfs = " ";
$codify_code = "";
$default_format = "%.5g";

sub col_mapping {
    local ($key, $n) = @_;

    die("dblib col_mapping: column name ``$key'' cannot begin with underscore.\n")
    	if ($key =~ /^\_/);
    die("dblib col_mapping: duplicate column name ``$key''\n")
    	if (defined($colnametonum{$key}));
    die ("dblib col_mapping: bad n.\n") if (!defined($n));
    $colnames[$n] = $key;
    $colnametonum{$key} = $n;
    $colnametonum{"_$key"} = $n;
    $colnametonum{"$n"} = $n;   # numeric synonyms
}

sub col_unmapping {
    local ($key) = @_;
    local ($n);
    $n = $colnametonum{$key};
    $colnames[$n] = undef if (defined($n));
    delete $colnametonum{$key};
    delete $colnametonum{"_$key"};
}

# Create a new column.
# Insert it before column $desired_n.
sub col_create {
    local ($key, $desired_n) = @_;
    local ($n, $i);
    die ("dblib col_create: called with duplicate column name ``$key''.\n")
    	if (defined($colnametonum{$key}));
    if (defined($desired_n)) {
    	# Shift columns over as necessary.
    	$n = $colnametonum{$desired_n};
	for ($i = $#colnames; $i >= $n; $i--) {
	    $tmp_key = $colnames[$i];
	    &col_unmapping($tmp_key);
	    &col_mapping($tmp_key, $i+1);
	};
    } else {
    	$n = $#colnames+1;
    };
    $colnames[$n] = $key;
    &col_mapping ($colnames[$n], $n);
    return $n;
}

sub fs_code_to_fsre_outfs {
    my($value) = @_;
    my($fsre, $outfs);
    if (!defined($value) || $value eq 'D') {  # default
	$fsre = "[ \t\n]+";
	$outfs = "\t";
    } elsif ($value eq 'S') {   # double space
	$fsre = '\s\s+';
	$outfs = "  ";
    } elsif ($value eq 't') {   # single tab
	$fsre = "\t";
	$outfs = "\t";
    } else {   # anything else
	$value = eval "qq{$value}";  # handle backslash expansion
	$fsre = "[$value]+";
	$outfs = $value;
    }
    return ($fsre, $outfs);
}

sub process_header {
    my($line, $headertag) = @_;
    $regexp = (defined($headertag) ? $headertag : $headertag_regexp);

    die ("dblib process_header: undefined header.\n\t$line\n")
	if (!defined($line));
    die ("dblib process_header: invalid header format: ``$line''.\n")
        if ($line !~ /^$regexp/);
    # perl-5.8.0 on a FRESH redhat 8.0 system give the error:
    # "Split loop, <STDIN> line 1."
    # on the next line (with the split)
    # if we directly assign to @colnames.
    #	        @colnames = split(/$header_fsre/, $line);
    # The problem does NOT happen if we instead
    # go through a temp variable, so we do that.
    # (It also does not happen if we use an upgraded RH8 system!)
    # Strange.
    my @c = split(/$header_fsre/, $line);
    @colnames = @c;
    shift @colnames;   # toss headertag
    @coloptions = ();
    #
    # handle options
    #
    while ($#colnames >= 0 && $colnames[0] =~ /^-(.)(.*)/) {
	push(@coloptions, shift @colnames);
	my($key, $value) = ($1, $2);
	if ($key eq 'F') {
	    ($fsre, $outfs) = fs_code_to_fsre_outfs($value);
	    $fs_code = $value;
	};
    };
    %colnametonum = ();
    foreach $i (0..$#colnames) {
    	&col_mapping ($colnames[$i], $i);
    };
}

sub readprocess_header {
    my($headertag) = @_;
    my($line);
    $line = <STDIN>;
    &process_header($line, $headertag);
}

sub write_header_fh_tag {
    my($FH) = shift;
    my($headertag) = shift;
    my(@cols) = @_;
    @cols = @colnames if ($#cols == -1);
    print $FH "$headertag$header_outfs" .
	($#coloptions != -1 ? join($header_outfs, @coloptions, '') : "") .
	join($header_outfs, @cols) .
	"\n";
}

sub write_header {
    write_header_fh_tag('STDOUT', $col_headertag, @_);
}

# listized
sub write_list_header {
    write_header_fh_tag('STDOUT', $list_headertag, @_);
}

sub escape_blanks {
    my($line) = @_;
    $line =~ s/[ \t]/_/g;
    return $line;
}

sub unescape_blanks {
    my($line) = @_;
    $line =~ s/_/ /g;
    return $line;
}

#
# codify:  convert db-code into perl code
#
# The conversion is a rename of all _foo's into
# database fields.
# For more perverse needs, _foo(N) means the Nth field after _foo.
# Also, as of 29-Jan-00, _last_foo gives the last row's value
# (_last_foo(N) is not supported).
# To convert we eval $codify_code.
#
# NEEDSWORK:  Should make some attempt to catch misspellings of column
# names.
#
sub codify {
    if ($codify_code eq "") {
        foreach (@colnames) {
	    $codify_code .= '$f =~ s/\b\_' . quotemeta($_) . '(\(.*\))/\$f\[' . $colnametonum{$_} . '+$1\]/g;' . "\n";
	    $codify_code .= '$f =~ s/\b\_' . quotemeta($_) . '\b/\$f\[' . $colnametonum{$_} . '\]/g;' . "\n";
	    $codify_code .= '$f =~ s/\b\_last\_' . quotemeta($_) . '\b/\$lf\[' . $colnametonum{$_} . '\]/g;' . "\n";
        };
    };
    my($f) = join(";", @_);
    eval $codify_code;
    return $f;
}

#
# code_prettify:  Convert db-code into "pretty code".
#
sub code_prettify {
    local($prettycode) = join(";", @_);
    $prettycode =~ s/\n/ /g;   # newlines will break commenting
    return $prettycode;
}

sub is_comment {
    return ($_ =~ /^\#/) || ($_ =~ /^\s*$/);
}

sub pass_comments {
    if (&is_comment) {
	print $_;
	return 1;
    };
    return 0;
}

sub delayed_pass_comments {
    if (&is_comment) {
	$delayed_comments = '' if (!defined($delayed_comments));
	$delayed_comments .= $_;
	return 1;
    };
    return 0;
}

sub delayed_write_comments {
    my($FH) = @_;
    $FH = 'STDOUT' if (!defined($FH));
    print $FH $delayed_comments if (defined($delayed_comments));
}

sub delayed_flush_comments {
    delayed_write_comments(@_);
    $delayed_comments = undef;
}

sub split_cols {
    chomp $_;
    @lf = @f;   # save the last columns
    @f = split(/$fsre/, $_);
}

sub write_cols {
    &write_these_cols_fh('STDOUT', @f);
};

sub write_these_cols {
    &write_these_cols_fh('STDOUT', @_);
};

sub write_these_cols_fh {
    my($FH) = shift;
    print $FH join($outfs, @_), "\n";
};

#
# output compare/entry code based on ARGV
# first entry is a sub:
#	sub row_col_fn {
#	    my($row, $colname, $n) = @_;
#	    # row is either a or b which we're comparing, or i for entries
#	    # colname is the user-given column name
#	    # n is 0..N of the cols to be sorted
#	}
# See the code in dbjoin and dbsort for implementations.
#
sub generate_compare_code {
    my($compare_function_name) = shift @_;
    my($row_col_fn) = shift @_;
    my(@args) = @_;
    my ($compare_code, $enter_code, $reverse, $numeric, $insensitive, $i);
    $compare_code = "sub $compare_function_name {\n";
    $enter_code = "";
    $reverse = 0;
    $numeric = 0;
    $insensitive = 0;
    $i = 0;
    foreach (@args) {
        if (/^-/) {
	    s/^-//;
	    my($options) = $_;
	    while ($options ne '') {
		$options =~ s/(.)//;
		($ch) = $1;
	        if ($ch eq 'r') { $reverse = 1; }
	        elsif ($ch eq 'R') { $reverse = 0; }
	        elsif ($ch eq 'n') { $numeric = 1; }
	        elsif ($ch eq 'N') { $numeric = 0; }
	        elsif ($ch eq 'i') { $insensitive = 1; die "$0: -i not yet supported\n"; }
	        elsif ($ch eq 'I') { $insensitive = 0; die "$0: -I not yet supported\n"; }
	        else { die "dblib generate_compare_code: unknown option $ch.\n"; };
	    };
	    next;
        };
	die ("dblib generate_compare_code: unknown column $_.\n") if (!defined($colnametonum{$_}));
	if ($reverse) {
            $first = 'b';   $second = 'a';
        } else {
            $first = 'a';   $second = 'b';
        };
        $compare_code .= '$r = (' . &$row_col_fn($first, $_, $i) . ' ' . 
    	    	    ($numeric ? "<=>" : "cmp") .
		    ' ' . &$row_col_fn($second, $_, $i) . '); ' . 
		    'return $r if ($r);' . 
		    " # $_" .
			($reverse && $numeric ? " (reversed, numeric)" :
			$reverse ? " (reversed)" :
			$numeric ? " (numeric)" :
			"") .
		    "\n";
        $enter_code .= &$row_col_fn('i', $_, $i) . 
       		    ' = $f[' . $colnametonum{$_} . '];' . "\n";
	$i++;
    }
    $compare_code .= "return 0;\n}";
    # Create the comparison function.
    eval $compare_code;
    $@ && die("dblib generate_compare_code: error ``$@ in'' eval of compare_code.\n$compare_code");
    return ($compare_code, $enter_code, $i-1);
}


sub abs {
    return $_[0] > 0 ? $_[0] : -$_[0];
}

sub progname {
    my($prog) = ($0);
    $prog =~ s@^.*/@@g;
    return $prog;
}

sub force_numeric {
    my($value, $ignore_non_numeric) = @_;
    if ($value =~ /^\s*[-+]?[0-9]+(.[0-9]+)?(e[-+0-9]+)?\s*$/) {
        return $value + 0.0;   # force numeric
    } else {
	if ($ignore_non_numeric) {
	    return undef;
	    next;
	} else {
	    return 0.0;
	};
    };
}

my($tmpfile_counter) = 0;
my(@tmpfiles) = ();
# call as tmpfile(FH)
sub db_tmpfile {
    my($fh) = @_;
    my($i) = $tmpfile_counter++;
    if ($i == 0) {
	 # install signals on first time
	 foreach (qw(HUP INT TERM)) {
	     $SIG{$_} = \&db_tmpfile_cleanup_signal;
	 };
    };
    my($fn) = &db_tmpdir . "/jdb.$$.$i";
    push(@tmpfiles, $fn);
    open($fh, "+>$fn") || die "$0: tmpfile open failed.\n";
    chmod 0600, $fn || die "$0: tmpfile chmod failed.\n";
    return $fn;
}
sub db_tmpfile_cleanup {
    my($fn) = @_;
    my(@new_tmpfiles);
    foreach (@tmpfiles) {
	push(@new_tmpfiles, $_) if ($_ ne $fn);
    };
    unlink($fn) == 1 or die "$0: cannot unlink tmpfiles.\n";
    @tmpfiles = @new_tmpfiles;
}
sub db_tmpfile_cleanup_all {
    foreach (@tmpfiles) {
	db_tmpfile_cleanup($_) if (-f $_);
    };
}
sub db_tmpfile_cleanup_signal {
    my($sig) = @_;
    # print "terminated with signal $sig, cleaning up ". join(',', @tmpfiles) . "\n";
    exit(1);
}

sub db_tmpdir {
    $ENV{'TMPDIR'} = '/tmp' if (!defined($ENV{'TMPDIR'}));
    return $ENV{'TMPDIR'};
}

my($dblib_date_inited) = undef;
sub dblib_date_init {
    eval "use HTTP::Date; use POSIX";
}

sub date_to_epoch {
    my($date) = @_;
    &dblib_date_init if (!$dblib_date_inited);
    return str2time($date);
}

sub epoch_to_date {
    my($epoch) = @_;
    &dblib_date_init if (!$dblib_date_inited);
    my($d) = strftime("%d-%b-%y", gmtime($epoch));
    $d =~ s/^0//;
    return $d;
}

sub epoch_to_fractional_year {
    my($epoch) = @_;
    &dblib_date_init if (!$dblib_date_inited);
    my($year) = strftime("%Y", gmtime($epoch));
    my($year_beg_epoch) = date_to_epoch("${year}0101");
    my($year_end_epoch) = date_to_epoch(($year+1) . "0101");
    my($year_secs) = $year_end_epoch - $year_beg_epoch;
    my($fraction) = ($epoch - $year_beg_epoch) / (1.0 * $year_secs);
    $fraction =~ s/^0//;
    return "$year$fraction";
}

sub END {
    &db_tmpfile_cleanup_all();
}


my(%iso_8859_1_to_html);
my($iso_8859_1_to_html_re_lhs);
sub init_dblib_text2html {
    $iso_8859_1_to_html_re_lhs = ('([');
    foreach (qw(38:amp 60:lt 62:gt
	    160:nbsp 161:iexcl 162:cent 163:pound 164:curren
	    165:yen 166:brvbar 167:sect 168:uml 169:copy
 	    170:ordf 171:laquo 172:not 173:shy 174:reg
 	    175:macr 176:deg 177:plusmn 178:sup2 179:sup3
 	    180:acute 181:micro 182:para 183:middot 184:cedil
 	    185:sup1 186:ordm 187:raquo 188:frac14 189:frac12
 	    190:frac34 191:iquest 192:Agrave 193:Aacute 194:Acirc
 	    195:Atilde 196:Auml 197:Aring 198:AElig 199:Ccedil
 	    200:Egrave 201:Eacute 202:Ecirc 203:Euml 204:Igrave
 	    205:Iacute 206:Icirc 207:Iuml 208:ETH 209:Ntilde
 	    210:Ograve 211:Oacute 212:Ocirc 213:Otilde 214:Ouml
 	    215:times 216:Oslash 217:Ugrave 218:Uacute 219:Ucirc
 	    220:Uuml 221:Yacute 222:THORN 223:szlig 224:agrave
 	    225:aacute 226:acirc 227:atilde 228:auml 229:aring
 	    230:aelig 231:ccedil 232:egrave 233:eacute 234:ecirc
 	    235:euml 236:igrave 237:iacute 238:icirc 239:iuml
 	    240:eth 241:ntilde 242:ograve 243:oacute 244:ocirc
 	    245:otilde 246:ouml 247:divide 248:oslash 249:ugrave
 	    250:uacute 251:ucirc 252:uuml 253:yacute 254:thorn
 	    255:yuml)) {
        my($num, $code) = split(/:/);
        $iso_8859_1_to_html{chr($num)} = "&$code;";
	if ($num > 128) {
	    my($oct) = sprintf("%o", $num);
	    $iso_8859_1_to_html{"\\$oct"} = "&$code;";
        };
	$iso_8859_1_to_html_re_lhs .= chr($num);
    }
    $iso_8859_1_to_html_re_lhs .= ']|\\\\[23][0-7][0-7])';
}

sub dblib_text2html {
    init_dblib_text2html if (!defined($iso_8859_1_to_html{'&'}));
    my($s) = @_;
    $s =~ s/$iso_8859_1_to_html_re_lhs/$iso_8859_1_to_html{$1}/ge;
    return $s;
}


1;
