#!/usr/bin/env ruby
# Copyright 2007-2008 Wincent Colaiuta
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

require File.join(File.dirname(__FILE__), 'spec_helper.rb')
require 'wikitext'

describe Wikitext::Parser, 'template parameter tags' do
  before do
    @parser = Wikitext::Parser.new
  end

  describe 'when referring to nonexistent variables' do
    it 'should not replace tags' do
      @parser.parse("{{{foo}}}").should == "<p>{{{foo}}}</p>\n"
    end
    
    it 'should escape HTML entities' do
      @parser.parse("{{{<script>}}}").should == "<p>{{{&lt;script&gt;}}}</p>\n"
    end
  end
  
  describe 'when included to the parser' do
    it 'should be included' do
      parameters = {'spam' => 'eggs'}
      parser = Wikitext::Parser.new(:parameters => parameters)
      parser.parse("{{{spam}}}").should == "<p>eggs</p>\n"
    end
  end
end
