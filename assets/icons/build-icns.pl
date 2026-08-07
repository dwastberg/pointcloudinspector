#!/usr/bin/env perl

use strict;
use warnings;

@ARGV == 2 or die "Usage: $0 INPUT.iconset OUTPUT.icns\n";
my ($iconset_dir, $output_path) = @ARGV;

# Modern ICNS chunks contain complete PNG files. Keep both standard and Retina
# representations even when two entries have the same pixel dimensions: their
# chunk types describe different logical sizes to macOS.
my @representations = (
    [ 'ic12', 'icon_32x32@2x.png' ],
    [ 'ic07', 'icon_128x128.png' ],
    [ 'ic13', 'icon_128x128@2x.png' ],
    [ 'ic08', 'icon_256x256.png' ],
    [ 'ic04', 'icon_16x16.png' ],
    [ 'ic14', 'icon_256x256@2x.png' ],
    [ 'ic09', 'icon_512x512.png' ],
    [ 'ic05', 'icon_32x32.png' ],
    [ 'ic10', 'icon_512x512@2x.png' ],
    [ 'ic11', 'icon_16x16@2x.png' ],
);

my @chunks;
my $total_size = 8;
for my $representation (@representations) {
    my ($type, $file_name) = @{$representation};
    my $path = "$iconset_dir/$file_name";
    open my $input, '<:raw', $path or die "Cannot read $path: $!\n";
    local $/;
    my $png = <$input>;
    close $input or die "Cannot close $path: $!\n";

    substr($png, 0, 8) eq "\x89PNG\r\n\x1a\n"
        or die "$path is not a PNG file\n";

    my $chunk = $type . pack('N', length($png) + 8) . $png;
    push @chunks, $chunk;
    $total_size += length($chunk);
}

my $temporary_path = "$output_path.tmp";
open my $output, '>:raw', $temporary_path
    or die "Cannot write $temporary_path: $!\n";
print {$output} 'icns', pack('N', $total_size), @chunks;
close $output or die "Cannot close $temporary_path: $!\n";
rename $temporary_path, $output_path
    or die "Cannot replace $output_path: $!\n";
